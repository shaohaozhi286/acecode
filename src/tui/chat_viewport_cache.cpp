#include "chat_viewport_cache.hpp"

#include <ftxui/screen/string.hpp>

#include <algorithm>
#include <string>
#include <utility>

namespace acecode::tui {
namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

std::uint64_t stable_hash(const std::string& value) {
    std::uint64_t hash = kFnvOffset;
    for (unsigned char c : value) {
        hash ^= c;
        hash *= kFnvPrime;
    }
    return hash;
}

void hash_combine(std::size_t& seed, std::size_t value) {
    seed ^= value + 0x9e3779b9u + (seed << 6) + (seed >> 2);
}

bool is_utf8_continuation(unsigned char c) {
    return (c & 0xC0) == 0x80;
}

std::size_t utf8_sequence_length(unsigned char c) {
    if ((c & 0x80) == 0x00) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

std::size_t next_codepoint_end(const std::string& s, std::size_t start) {
    if (start >= s.size()) return s.size();
    const std::size_t len =
        utf8_sequence_length(static_cast<unsigned char>(s[start]));
    const std::size_t end = start + len;
    if (end > s.size()) return start + 1;
    for (std::size_t i = start + 1; i < end; ++i) {
        if (!is_utf8_continuation(static_cast<unsigned char>(s[i]))) {
            return start + 1;
        }
    }
    return end;
}

int visual_width(const std::string& s) {
    return std::max(0, ftxui::string_width(s));
}

std::string spaces_for_visual_width(int width) {
    return std::string(static_cast<std::size_t>(std::max(0, width)), ' ');
}

std::vector<std::string> wrap_display_text(const std::string& text,
                                           int content_width) {
    const int width = std::max(1, content_width);
    std::vector<std::string> out;
    std::string current;
    int current_width = 0;

    auto flush = [&]() {
        out.push_back(current);
        current.clear();
        current_width = 0;
    };

    for (std::size_t pos = 0; pos < text.size();) {
        if (text[pos] == '\r') {
            ++pos;
            continue;
        }
        if (text[pos] == '\n') {
            flush();
            ++pos;
            continue;
        }

        const std::size_t end = next_codepoint_end(text, pos);
        std::string piece = text.substr(pos, end - pos);
        const int piece_width = std::max(1, visual_width(piece));
        if (current_width > 0 && current_width + piece_width > width) {
            flush();
        }
        current += piece;
        current_width += piece_width;
        pos = end;
    }

    if (!current.empty() || out.empty()) {
        flush();
    }
    return out;
}

int count_wrapped_display_rows(const std::string& text,
                               int content_width) {
    const int width = std::max(1, content_width);
    int rows = 0;
    int current_width = 0;
    bool saw_content = false;

    auto flush = [&]() {
        ++rows;
        current_width = 0;
    };

    for (std::size_t pos = 0; pos < text.size();) {
        if (text[pos] == '\r') {
            ++pos;
            continue;
        }
        if (text[pos] == '\n') {
            flush();
            saw_content = false;
            ++pos;
            continue;
        }

        const std::size_t end = next_codepoint_end(text, pos);
        const std::string piece = text.substr(pos, end - pos);
        const int piece_width = std::max(1, visual_width(piece));
        if (current_width > 0 && current_width + piece_width > width) {
            flush();
        }
        current_width += piece_width;
        saw_content = true;
        pos = end;
    }

    if (saw_content || rows == 0) {
        flush();
    }
    return rows;
}

ChatViewportRowStyle style_for_role(const std::string& role) {
    if (role == "user") return ChatViewportRowStyle::User;
    if (role == "tool_call") return ChatViewportRowStyle::ToolCall;
    if (role == "tool_result" || role == "user_shell_output") {
        return ChatViewportRowStyle::ToolResult;
    }
    if (role == "system") return ChatViewportRowStyle::System;
    if (role == "error") return ChatViewportRowStyle::Error;
    return ChatViewportRowStyle::Assistant;
}

std::string prefix_for_role(const std::string& role) {
    if (role == "user") return " > ";
    if (role == "assistant") return " * ";
    if (role == "tool_call") return "   -> ";
    if (role == "tool_result" || role == "user_shell_output") return "   <- ";
    if (role == "system") return " i ";
    if (role == "error") return " ! ";
    return " * ";
}

std::string display_text_for_message(const ChatViewportMessageInput& message) {
    if (message.role == "tool_call" && !message.display_override.empty()) {
        return message.display_override;
    }
    if (message.role == "tool_result" && message.has_summary &&
        !message.expanded && !message.display_override.empty()) {
        return message.display_override;
    }
    return message.content;
}

} // namespace

std::size_t ChatViewportLayoutCache::KeyHash::operator()(
    const ChatViewportLayoutKey& key) const {
    std::size_t seed = 0;
    hash_combine(seed, std::hash<int>{}(key.message_index));
    hash_combine(seed, std::hash<int>{}(key.content_width));
    hash_combine(seed, std::hash<std::string>{}(key.role));
    hash_combine(seed, std::hash<std::uint64_t>{}(key.content_hash));
    hash_combine(seed, std::hash<std::uint64_t>{}(key.display_override_hash));
    hash_combine(seed, std::hash<std::uint64_t>{}(key.layout_version_hash));
    hash_combine(seed, std::hash<std::size_t>{}(key.content_size));
    hash_combine(seed, std::hash<std::size_t>{}(key.display_override_size));
    hash_combine(seed, std::hash<bool>{}(key.expanded));
    hash_combine(seed, std::hash<bool>{}(key.has_summary));
    hash_combine(seed, std::hash<bool>{}(key.has_hunks));
    return seed;
}

ChatViewportLayoutKey chat_viewport_layout_key_for_message(
    int message_index,
    const ChatViewportMessageInput& message,
    int content_width) {
    ChatViewportLayoutKey key;
    key.message_index = message_index;
    key.content_width = std::max(1, content_width);
    key.role = message.role;
    key.content_hash = stable_hash(message.content);
    key.display_override_hash = stable_hash(message.display_override);
    key.layout_version_hash = stable_hash(message.layout_version);
    key.content_size = message.content.size();
    key.display_override_size = message.display_override.size();
    key.expanded = message.expanded;
    key.has_summary = message.has_summary;
    key.has_hunks = message.has_hunks;
    return key;
}

ChatViewportCachedLayout chat_viewport_build_layout(
    const ChatViewportLayoutKey& key,
    const ChatViewportMessageInput& message) {
    ChatViewportCachedLayout layout;
    layout.key = key;

    const std::string prefix = prefix_for_role(message.role);
    const std::string continuation_prefix =
        spaces_for_visual_width(visual_width(prefix));
    const int content_width =
        std::max(1, key.content_width - visual_width(prefix));
    const auto lines = wrap_display_text(display_text_for_message(message),
                                         content_width);
    const auto style = style_for_role(message.role);

    layout.rows.reserve(lines.size());
    for (std::size_t i = 0; i < lines.size(); ++i) {
        ChatViewportCachedRow row;
        row.message_index = key.message_index;
        row.message_row = static_cast<int>(i);
        row.prefix = i == 0 ? prefix : continuation_prefix;
        row.text = lines[i];
        row.style = style;
        layout.rows.push_back(std::move(row));
    }
    return layout;
}

int chat_viewport_count_layout_rows(
    const ChatViewportLayoutKey& key,
    const ChatViewportMessageInput& message) {
    const std::string prefix = prefix_for_role(message.role);
    const int content_width =
        std::max(1, key.content_width - visual_width(prefix));
    return count_wrapped_display_rows(display_text_for_message(message),
                                      content_width);
}

std::vector<ChatViewportCachedRow> chat_viewport_slice_rows(
    const ChatViewportCachedLayout& layout,
    int row_begin,
    int row_end) {
    const int begin = std::clamp(row_begin, 0, layout.row_count());
    const int end = std::clamp(row_end, begin, layout.row_count());
    return std::vector<ChatViewportCachedRow>(
        layout.rows.begin() + begin, layout.rows.begin() + end);
}

const ChatViewportCachedLayout& ChatViewportLayoutCache::layout_for_message(
    int message_index,
    const ChatViewportMessageInput& message,
    int content_width) {
    const auto key = chat_viewport_layout_key_for_message(
        message_index, message, content_width);
    auto found = layouts_.find(key);
    if (found != layouts_.end()) {
        ++stats_.hits;
        return found->second;
    }

    ++stats_.misses;
    ++stats_.builds;
    erase_message(message_index);
    auto layout = chat_viewport_build_layout(key, message);
    row_counts_[key] = layout.row_count();
    auto inserted = layouts_.emplace(key, std::move(layout));
    return inserted.first->second;
}

int ChatViewportLayoutCache::row_count_for_message(
    int message_index,
    const ChatViewportMessageInput& message,
    int content_width) {
    const auto key = chat_viewport_layout_key_for_message(
        message_index, message, content_width);
    auto found = row_counts_.find(key);
    if (found != row_counts_.end()) {
        ++stats_.row_count_hits;
        return found->second;
    }

    ++stats_.row_count_misses;
    ++stats_.row_count_builds;
    erase_message(message_index);
    const int rows = chat_viewport_count_layout_rows(key, message);
    row_counts_[key] = rows;
    return rows;
}

void ChatViewportLayoutCache::erase_message(int message_index) {
    for (auto it = layouts_.begin(); it != layouts_.end();) {
        if (it->first.message_index == message_index) {
            it = layouts_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = row_counts_.begin(); it != row_counts_.end();) {
        if (it->first.message_index == message_index) {
            it = row_counts_.erase(it);
        } else {
            ++it;
        }
    }
}

void ChatViewportLayoutCache::clear() {
    layouts_.clear();
    row_counts_.clear();
    stats_ = {};
}

bool ChatViewportLayoutCache::contains(
    const ChatViewportLayoutKey& key) const {
    return layouts_.find(key) != layouts_.end();
}

std::size_t ChatViewportLayoutCache::size() const {
    return layouts_.size();
}

ChatViewportCacheStats ChatViewportLayoutCache::stats() const {
    return stats_;
}

} // namespace acecode::tui
