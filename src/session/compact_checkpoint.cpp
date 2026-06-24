#include "compact_checkpoint.hpp"
#include "session_serializer.hpp"
#include "../utils/uuid.hpp"

#include <nlohmann/json.hpp>

namespace acecode {
namespace {

bool is_transcript_only_message(const ChatMessage& msg) {
    return msg.metadata.is_object() &&
           msg.metadata.value("transcript_only", false);
}

bool is_provider_role(const std::string& role) {
    return role == "system" || role == "user" ||
           role == "assistant" || role == "tool";
}

nlohmann::json message_to_json(const ChatMessage& msg) {
    return nlohmann::json::parse(serialize_message(msg));
}

std::optional<ChatMessage> message_from_json(const nlohmann::json& json) {
    if (!json.is_object()) return std::nullopt;
    try {
        return deserialize_message(json.dump());
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

bool is_compact_checkpoint_message(const ChatMessage& msg) {
    return msg.is_meta && msg.subtype == kCompactCheckpointSubtype;
}

ChatMessage encode_compact_checkpoint(const CompactCheckpoint& checkpoint) {
    ChatMessage msg;
    msg.role = "system";
    msg.content = "[Compact checkpoint]";
    msg.uuid = checkpoint.id.empty() ? generate_uuid() : checkpoint.id;
    msg.subtype = kCompactCheckpointSubtype;
    msg.timestamp = checkpoint.timestamp.empty() ? iso_timestamp() : checkpoint.timestamp;
    msg.is_meta = true;

    nlohmann::json replacement = nlohmann::json::array();
    for (const auto& item : checkpoint.replacement_history) {
        replacement.push_back(message_to_json(item));
    }

    msg.metadata = nlohmann::json{
        {"version", checkpoint.version},
        {"id", msg.uuid},
        {"timestamp", msg.timestamp},
        {"trigger", checkpoint.trigger},
        {"summary", checkpoint.summary},
        {"messages_compressed", checkpoint.messages_compressed},
        {"estimated_tokens_saved", checkpoint.estimated_tokens_saved},
        {"pre_tokens", checkpoint.pre_tokens},
        {"post_tokens", checkpoint.post_tokens},
        {"replacement_history", std::move(replacement)},
    };
    return msg;
}

std::optional<CompactCheckpoint> decode_compact_checkpoint(const ChatMessage& msg) {
    if (!is_compact_checkpoint_message(msg) || !msg.metadata.is_object()) {
        return std::nullopt;
    }
    const auto& metadata = msg.metadata;
    if (!metadata.contains("replacement_history") ||
        !metadata["replacement_history"].is_array()) {
        return std::nullopt;
    }

    CompactCheckpoint checkpoint;
    checkpoint.version = metadata.value("version", kCompactCheckpointVersion);
    checkpoint.id = metadata.value("id", msg.uuid);
    checkpoint.timestamp = metadata.value("timestamp", msg.timestamp);
    checkpoint.trigger = metadata.value("trigger", std::string{});
    checkpoint.summary = metadata.value("summary", std::string{});
    checkpoint.messages_compressed = metadata.value("messages_compressed", 0);
    checkpoint.estimated_tokens_saved = metadata.value("estimated_tokens_saved", 0);
    checkpoint.pre_tokens = metadata.value("pre_tokens", 0);
    checkpoint.post_tokens = metadata.value("post_tokens", 0);

    for (const auto& item : metadata["replacement_history"]) {
        auto decoded = message_from_json(item);
        if (!decoded.has_value()) {
            return std::nullopt;
        }
        checkpoint.replacement_history.push_back(std::move(*decoded));
    }
    return checkpoint;
}

std::vector<ChatMessage> provider_relevant_messages(const std::vector<ChatMessage>& messages) {
    std::vector<ChatMessage> result;
    result.reserve(messages.size());
    for (const auto& msg : messages) {
        if (is_compact_checkpoint_message(msg)) continue;
        if (msg.is_meta) continue;
        if (is_transcript_only_message(msg)) continue;
        if (!is_provider_role(msg.role)) continue;
        result.push_back(msg);
    }
    return result;
}

std::vector<ChatMessage> reconstruct_effective_model_history(
    const std::vector<ChatMessage>& raw_messages) {
    std::vector<ChatMessage> effective;
    std::size_t suffix_start = 0;

    for (std::size_t i = raw_messages.size(); i > 0; --i) {
        const auto checkpoint = decode_compact_checkpoint(raw_messages[i - 1]);
        if (!checkpoint.has_value()) continue;
        effective = checkpoint->replacement_history;
        suffix_start = i;
        break;
    }

    if (effective.empty() && suffix_start == 0) {
        return provider_relevant_messages(raw_messages);
    }

    for (std::size_t i = suffix_start; i < raw_messages.size(); ++i) {
        const auto& msg = raw_messages[i];
        if (is_compact_checkpoint_message(msg)) {
            auto checkpoint = decode_compact_checkpoint(msg);
            if (checkpoint.has_value()) {
                effective = checkpoint->replacement_history;
            }
            continue;
        }
        auto one = provider_relevant_messages({msg});
        effective.insert(effective.end(), one.begin(), one.end());
    }
    return effective;
}

} // namespace acecode
