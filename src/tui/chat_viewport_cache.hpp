#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace acecode::tui {

enum class ChatViewportRowStyle {
    User,
    Assistant,
    ToolCall,
    ToolResult,
    System,
    Error,
};

struct ChatViewportMessageInput {
    std::string role;
    std::string content;
    std::string display_override;
    std::string layout_version;
    bool expanded = false;
    bool has_summary = false;
    bool has_hunks = false;

    bool operator==(const ChatViewportMessageInput& other) const {
        return role == other.role &&
               content == other.content &&
               display_override == other.display_override &&
               layout_version == other.layout_version &&
               expanded == other.expanded &&
               has_summary == other.has_summary &&
               has_hunks == other.has_hunks;
    }
};

struct ChatViewportLayoutKey {
    int message_index = -1;
    int content_width = 0;
    std::string role;
    std::uint64_t content_hash = 0;
    std::uint64_t display_override_hash = 0;
    std::uint64_t layout_version_hash = 0;
    std::size_t content_size = 0;
    std::size_t display_override_size = 0;
    bool expanded = false;
    bool has_summary = false;
    bool has_hunks = false;

    bool operator==(const ChatViewportLayoutKey& other) const {
        return message_index == other.message_index &&
               content_width == other.content_width &&
               role == other.role &&
               content_hash == other.content_hash &&
               display_override_hash == other.display_override_hash &&
               layout_version_hash == other.layout_version_hash &&
               content_size == other.content_size &&
               display_override_size == other.display_override_size &&
               expanded == other.expanded &&
               has_summary == other.has_summary &&
               has_hunks == other.has_hunks;
    }
};

struct ChatViewportCachedRow {
    int message_index = -1;
    int message_row = 0;
    std::string prefix;
    std::string text;
    ChatViewportRowStyle style = ChatViewportRowStyle::Assistant;
};

struct ChatViewportCachedLayout {
    ChatViewportLayoutKey key;
    std::vector<ChatViewportCachedRow> rows;

    int row_count() const {
        return static_cast<int>(rows.size());
    }
};

struct ChatViewportCacheStats {
    int hits = 0;
    int misses = 0;
    int builds = 0;
    int row_count_hits = 0;
    int row_count_misses = 0;
    int row_count_builds = 0;
};

ChatViewportLayoutKey chat_viewport_layout_key_for_message(
    int message_index,
    const ChatViewportMessageInput& message,
    int content_width);

ChatViewportCachedLayout chat_viewport_build_layout(
    const ChatViewportLayoutKey& key,
    const ChatViewportMessageInput& message);

int chat_viewport_count_layout_rows(
    const ChatViewportLayoutKey& key,
    const ChatViewportMessageInput& message);

std::vector<ChatViewportCachedRow> chat_viewport_slice_rows(
    const ChatViewportCachedLayout& layout,
    int row_begin,
    int row_end);

class ChatViewportLayoutCache {
public:
    const ChatViewportCachedLayout& layout_for_message(
        int message_index,
        const ChatViewportMessageInput& message,
        int content_width);

    int row_count_for_message(
        int message_index,
        const ChatViewportMessageInput& message,
        int content_width);

    void erase_message(int message_index);
    void clear();

    bool contains(const ChatViewportLayoutKey& key) const;
    std::size_t size() const;
    ChatViewportCacheStats stats() const;

private:
    struct KeyHash {
        std::size_t operator()(const ChatViewportLayoutKey& key) const;
    };

    std::unordered_map<ChatViewportLayoutKey,
                       ChatViewportCachedLayout,
                       KeyHash> layouts_;
    std::unordered_map<ChatViewportLayoutKey, int, KeyHash> row_counts_;
    ChatViewportCacheStats stats_;
};

} // namespace acecode::tui
