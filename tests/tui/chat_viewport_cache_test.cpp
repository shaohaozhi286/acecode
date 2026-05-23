#include <gtest/gtest.h>

#include "tui/chat_viewport_cache.hpp"

using acecode::tui::ChatViewportLayoutCache;
using acecode::tui::ChatViewportMessageInput;
using acecode::tui::ChatViewportRowStyle;
using acecode::tui::chat_viewport_build_layout;
using acecode::tui::chat_viewport_layout_key_for_message;
using acecode::tui::chat_viewport_slice_rows;

TEST(ChatViewportCache, StableMessageReusesCachedLayout) {
    ChatViewportLayoutCache cache;
    ChatViewportMessageInput message;
    message.role = "assistant";
    message.content = "one two three four five six";

    const auto& first = cache.layout_for_message(0, message, 16);
    const int first_rows = first.row_count();
    const auto& second = cache.layout_for_message(0, message, 16);

    EXPECT_EQ(first_rows, second.row_count());
    EXPECT_EQ(cache.size(), 1u);
    EXPECT_EQ(cache.stats().builds, 1);
    EXPECT_EQ(cache.stats().hits, 1);
    EXPECT_EQ(cache.stats().misses, 1);
}

TEST(ChatViewportCache, WidthChangeRebuildsAndRewrapsMessage) {
    ChatViewportLayoutCache cache;
    ChatViewportMessageInput message;
    message.role = "assistant";
    message.content = "abcdefghijabcdefghij";

    const auto& narrow = cache.layout_for_message(0, message, 8);
    const int narrow_rows = narrow.row_count();
    const auto& wide = cache.layout_for_message(0, message, 24);

    EXPECT_GT(narrow_rows, wide.row_count());
    EXPECT_EQ(cache.size(), 1u);
    EXPECT_EQ(cache.stats().builds, 2);
}

TEST(ChatViewportCache, RowCountCacheAvoidsMaterializingRows) {
    ChatViewportLayoutCache cache;
    ChatViewportMessageInput message;
    message.role = "assistant";
    message.content = "one two three four five six";

    EXPECT_GT(cache.row_count_for_message(0, message, 10), 1);
    EXPECT_EQ(cache.size(), 0u);
    EXPECT_EQ(cache.stats().row_count_builds, 1);

    EXPECT_GT(cache.row_count_for_message(0, message, 10), 1);
    EXPECT_EQ(cache.stats().row_count_hits, 1);
    EXPECT_EQ(cache.stats().row_count_builds, 1);
}

TEST(ChatViewportCache, WrapsWideCjkGlyphsByTerminalCellWidth) {
    ChatViewportMessageInput message;
    message.role = "assistant";
    message.content = "中文中文";

    auto key = chat_viewport_layout_key_for_message(0, message, 7);
    auto layout = chat_viewport_build_layout(key, message);

    ASSERT_EQ(layout.rows.size(), 2u);
    EXPECT_EQ(layout.rows[0].text, "中文");
    EXPECT_EQ(layout.rows[1].text, "中文");
}

TEST(ChatViewportCache, ExpandedStateAndLayoutVersionInvalidateKey) {
    ChatViewportMessageInput collapsed;
    collapsed.role = "tool_result";
    collapsed.content = "summary row";
    collapsed.has_summary = true;

    ChatViewportMessageInput expanded = collapsed;
    expanded.expanded = true;

    auto collapsed_key = chat_viewport_layout_key_for_message(
        2, collapsed, 40);
    auto expanded_key = chat_viewport_layout_key_for_message(
        2, expanded, 40);
    EXPECT_FALSE(collapsed_key == expanded_key);

    expanded.layout_version = "hunks:v2";
    auto version_key = chat_viewport_layout_key_for_message(
        2, expanded, 40);
    EXPECT_FALSE(expanded_key == version_key);
}

TEST(ChatViewportCache, StreamingTailOnlyRebuildsChangedMessage) {
    ChatViewportLayoutCache cache;
    ChatViewportMessageInput stable;
    stable.role = "user";
    stable.content = "stable prompt";
    ChatViewportMessageInput tail;
    tail.role = "assistant";
    tail.content = "stream";

    cache.layout_for_message(0, stable, 40);
    cache.layout_for_message(1, tail, 40);
    EXPECT_EQ(cache.stats().builds, 2);

    tail.content += " append";
    cache.layout_for_message(0, stable, 40);
    cache.layout_for_message(1, tail, 40);

    EXPECT_EQ(cache.size(), 2u);
    EXPECT_EQ(cache.stats().hits, 1);
    EXPECT_EQ(cache.stats().builds, 3);
}

TEST(ChatViewportCache, ConvertsKnownRolesToViewportRows) {
    struct Case {
        const char* role;
        ChatViewportRowStyle style;
        const char* prefix;
    };
    const Case cases[] = {
        {"user", ChatViewportRowStyle::User, " > "},
        {"assistant", ChatViewportRowStyle::Assistant, " * "},
        {"tool_call", ChatViewportRowStyle::ToolCall, "   -> "},
        {"tool_result", ChatViewportRowStyle::ToolResult, "   <- "},
        {"system", ChatViewportRowStyle::System, " i "},
        {"error", ChatViewportRowStyle::Error, " ! "},
    };

    for (const auto& c : cases) {
        ChatViewportMessageInput message;
        message.role = c.role;
        message.content = "body";
        auto key = chat_viewport_layout_key_for_message(0, message, 40);
        auto layout = chat_viewport_build_layout(key, message);
        ASSERT_EQ(layout.rows.size(), 1u) << c.role;
        EXPECT_EQ(layout.rows[0].style, c.style) << c.role;
        EXPECT_EQ(layout.rows[0].prefix, c.prefix) << c.role;
    }
}

TEST(ChatViewportCache, ToolCallUsesDisplayOverrideForVisibleRows) {
    ChatViewportMessageInput message;
    message.role = "tool_call";
    message.content = "{\"cmd\":\"long json\"}";
    message.display_override = "bash npm test";

    auto key = chat_viewport_layout_key_for_message(3, message, 40);
    auto layout = chat_viewport_build_layout(key, message);

    ASSERT_EQ(layout.rows.size(), 1u);
    EXPECT_EQ(layout.rows[0].text, "bash npm test");
}

TEST(ChatViewportCache, CollapsedToolResultUsesSummaryDisplayOverride) {
    ChatViewportMessageInput message;
    message.role = "tool_result";
    message.content = "full output line 1\nfull output line 2";
    message.display_override = "Read file.txt · 12 lines";
    message.has_summary = true;

    auto key = chat_viewport_layout_key_for_message(3, message, 80);
    auto layout = chat_viewport_build_layout(key, message);

    ASSERT_EQ(layout.rows.size(), 1u);
    EXPECT_EQ(layout.rows[0].text, "Read file.txt · 12 lines");

    message.expanded = true;
    key = chat_viewport_layout_key_for_message(3, message, 80);
    layout = chat_viewport_build_layout(key, message);
    ASSERT_EQ(layout.rows.size(), 2u);
    EXPECT_EQ(layout.rows[0].text, "full output line 1");
    EXPECT_EQ(layout.rows[1].text, "full output line 2");
}

TEST(ChatViewportCache, SlicesVisibleRowsByMessageLocalRange) {
    ChatViewportMessageInput message;
    message.role = "assistant";
    message.content = "aaaa\nbbbb\ncccc\ndddd";
    auto key = chat_viewport_layout_key_for_message(4, message, 80);
    auto layout = chat_viewport_build_layout(key, message);

    auto slice = chat_viewport_slice_rows(layout, 1, 3);
    ASSERT_EQ(slice.size(), 2u);
    EXPECT_EQ(slice[0].message_row, 1);
    EXPECT_EQ(slice[0].text, "bbbb");
    EXPECT_EQ(slice[1].message_row, 2);
    EXPECT_EQ(slice[1].text, "cccc");
}
