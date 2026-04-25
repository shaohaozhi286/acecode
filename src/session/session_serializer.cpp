#include "session_serializer.hpp"
#include <nlohmann/json.hpp>

namespace acecode {

std::string serialize_message(const ChatMessage& msg) {
    nlohmann::json j;
    j["role"] = msg.role;

    if (!msg.content.empty()) {
        j["content"] = msg.content;
    }

    if (!msg.tool_calls.is_null() && !msg.tool_calls.empty()) {
        j["tool_calls"] = msg.tool_calls;
    }

    if (!msg.tool_call_id.empty()) {
        j["tool_call_id"] = msg.tool_call_id;
    }

    // Reasoning chain-of-thought from DeepSeek thinking mode et al. Persisted
    // so --resume can echo it back on the next turn (otherwise the next API
    // call after resume would 400 with "must be passed back to the API").
    if (!msg.reasoning_content.empty()) {
        j["reasoning_content"] = msg.reasoning_content;
    }

    // Compact pipeline metadata fields (omit when default)
    if (!msg.uuid.empty()) {
        j["uuid"] = msg.uuid;
    }
    if (!msg.subtype.empty()) {
        j["subtype"] = msg.subtype;
    }
    if (!msg.timestamp.empty()) {
        j["timestamp"] = msg.timestamp;
    }
    if (msg.is_meta) {
        j["is_meta"] = true;
    }
    if (msg.is_compact_summary) {
        j["is_compact_summary"] = true;
    }
    if (!msg.metadata.is_null() && !msg.metadata.empty()) {
        j["metadata"] = msg.metadata;
    }

    // dump(-1) produces compact single-line JSON with no extra whitespace
    return j.dump(-1);
}

ChatMessage deserialize_message(const std::string& line) {
    auto j = nlohmann::json::parse(line);
    ChatMessage msg;

    if (j.contains("role") && j["role"].is_string()) {
        msg.role = j["role"].get<std::string>();
    }

    if (j.contains("content") && j["content"].is_string()) {
        msg.content = j["content"].get<std::string>();
    }

    if (j.contains("tool_calls") && j["tool_calls"].is_array()) {
        msg.tool_calls = j["tool_calls"];
    }

    if (j.contains("tool_call_id") && j["tool_call_id"].is_string()) {
        msg.tool_call_id = j["tool_call_id"].get<std::string>();
    }

    // Optional — absent on every JSONL line written before this change.
    if (j.contains("reasoning_content") && j["reasoning_content"].is_string()) {
        msg.reasoning_content = j["reasoning_content"].get<std::string>();
    }

    // Compact pipeline metadata fields (graceful defaults for legacy JSONL)
    if (j.contains("uuid") && j["uuid"].is_string()) {
        msg.uuid = j["uuid"].get<std::string>();
    }
    if (j.contains("subtype") && j["subtype"].is_string()) {
        msg.subtype = j["subtype"].get<std::string>();
    }
    if (j.contains("timestamp") && j["timestamp"].is_string()) {
        msg.timestamp = j["timestamp"].get<std::string>();
    }
    if (j.contains("is_meta") && j["is_meta"].is_boolean()) {
        msg.is_meta = j["is_meta"].get<bool>();
    }
    if (j.contains("is_compact_summary") && j["is_compact_summary"].is_boolean()) {
        msg.is_compact_summary = j["is_compact_summary"].get<bool>();
    }
    if (j.contains("metadata") && j["metadata"].is_object()) {
        msg.metadata = j["metadata"];
    }

    return msg;
}

} // namespace acecode
