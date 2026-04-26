#include "session_rewind.hpp"

#include "../utils/uuid.hpp"

#include <algorithm>

namespace acecode {

namespace {

bool starts_with(const std::string& s, const std::string& prefix) {
    return s.rfind(prefix, 0) == 0;
}

bool contains_any_synthetic_tag(const std::string& s) {
    static const char* kTags[] = {
        "<bash-input>",
        "<bash-stdout>",
        "<bash-stderr>",
        "<bash-exit-code>",
        "<local-command-stdout>",
        "<local-command-stderr>",
        "<task-notification>",
        "<tick>",
        "<teammate-message",
    };
    for (const char* tag : kTags) {
        if (s.find(tag) != std::string::npos) return true;
    }
    return false;
}

std::string collapse_ws(std::string s) {
    bool in_space = false;
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
        const bool space = (ch == '\n' || ch == '\r' || ch == '\t' || ch == ' ');
        if (space) {
            if (!in_space) out.push_back(' ');
            in_space = true;
        } else {
            out.push_back(ch);
            in_space = false;
        }
    }
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

} // namespace

void ensure_user_message_identity(ChatMessage& msg) {
    if (msg.role != "user") return;
    if (msg.uuid.empty()) msg.uuid = generate_uuid();
    if (msg.timestamp.empty()) msg.timestamp = iso_timestamp();
}

bool is_file_checkpoint_message(const ChatMessage& msg) {
    return msg.is_meta && msg.subtype == "file_checkpoint";
}

bool is_rewind_selectable_user_message(const ChatMessage& msg) {
    if (msg.role != "user") return false;
    if (msg.is_meta || msg.is_compact_summary) return false;
    if (msg.content.empty()) return false;
    if (starts_with(msg.content, "!")) return false;
    if (contains_any_synthetic_tag(msg.content)) return false;
    return true;
}

std::vector<RewindTarget> collect_rewind_targets(const std::vector<ChatMessage>& messages) {
    std::vector<RewindTarget> out;
    for (size_t i = 0; i < messages.size(); ++i) {
        const auto& msg = messages[i];
        if (!is_rewind_selectable_user_message(msg)) continue;
        RewindTarget target;
        target.message_index = i;
        target.message_uuid = msg.uuid;
        target.preview = rewind_preview_text(msg);
        target.has_stable_uuid = !msg.uuid.empty();
        out.push_back(std::move(target));
    }
    return out;
}

std::vector<ChatMessage> retained_prefix_before_index(
    const std::vector<ChatMessage>& messages,
    size_t target_index) {
    const size_t end = std::min(target_index, messages.size());
    return std::vector<ChatMessage>(messages.begin(), messages.begin() + static_cast<std::ptrdiff_t>(end));
}

std::string rewind_prefill_text(const ChatMessage& msg) {
    if (!is_rewind_selectable_user_message(msg)) return {};
    return msg.content;
}

std::string rewind_preview_text(const ChatMessage& msg, size_t max_bytes) {
    std::string s = collapse_ws(msg.content);
    if (s.size() <= max_bytes) return s;
    if (max_bytes <= 3) return s.substr(0, max_bytes);
    return s.substr(0, max_bytes - 3) + "...";
}

} // namespace acecode
