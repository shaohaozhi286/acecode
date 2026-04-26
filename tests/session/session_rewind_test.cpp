#include <gtest/gtest.h>

#include "provider/llm_provider.hpp"
#include "session/session_rewind.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

using acecode::ChatMessage;
using acecode::collect_rewind_targets;
using acecode::ensure_user_message_identity;
using acecode::is_rewind_selectable_user_message;
using acecode::retained_prefix_before_index;
using acecode::rewind_prefill_text;

namespace {

ChatMessage user(std::string content) {
    ChatMessage msg;
    msg.role = "user";
    msg.content = std::move(content);
    return msg;
}

} // namespace

TEST(SessionRewind, AssignsIdentityToUserMessagesOnly) {
    ChatMessage msg = user("hello");
    ensure_user_message_identity(msg);

    EXPECT_FALSE(msg.uuid.empty());
    EXPECT_FALSE(msg.timestamp.empty());

    ChatMessage assistant;
    assistant.role = "assistant";
    assistant.content = "hi";
    ensure_user_message_identity(assistant);
    EXPECT_TRUE(assistant.uuid.empty());
    EXPECT_TRUE(assistant.timestamp.empty());
}

TEST(SessionRewind, SelectableUserMessagesExcludeSyntheticTurns) {
    ChatMessage normal = user("fix the parser");
    EXPECT_TRUE(is_rewind_selectable_user_message(normal));

    ChatMessage meta = user("meta");
    meta.is_meta = true;
    EXPECT_FALSE(is_rewind_selectable_user_message(meta));

    ChatMessage compact = user("summary");
    compact.is_compact_summary = true;
    EXPECT_FALSE(is_rewind_selectable_user_message(compact));

    EXPECT_FALSE(is_rewind_selectable_user_message(user("!git status")));
    EXPECT_FALSE(is_rewind_selectable_user_message(user("<bash-input>ls</bash-input>")));
    EXPECT_FALSE(is_rewind_selectable_user_message(user("<task-notification>done</task-notification>")));
    EXPECT_FALSE(is_rewind_selectable_user_message(user("")));
}

TEST(SessionRewind, LegacyMessagesRemainSelectableWithoutCodeAnchor) {
    std::vector<ChatMessage> messages;
    messages.push_back(user("legacy prompt"));
    messages.push_back(user("stable prompt"));
    messages.back().uuid = "u2";

    auto targets = collect_rewind_targets(messages);

    ASSERT_EQ(targets.size(), 2u);
    EXPECT_EQ(targets[0].message_index, 0u);
    EXPECT_FALSE(targets[0].has_stable_uuid);
    EXPECT_EQ(targets[1].message_index, 1u);
    EXPECT_TRUE(targets[1].has_stable_uuid);
    EXPECT_EQ(targets[1].message_uuid, "u2");
}

TEST(SessionRewind, RetainedPrefixStopsBeforeTarget) {
    std::vector<ChatMessage> messages;
    messages.push_back(user("one"));
    ChatMessage assistant;
    assistant.role = "assistant";
    assistant.content = "answer";
    messages.push_back(assistant);
    messages.push_back(user("two"));

    auto prefix = retained_prefix_before_index(messages, 2);

    ASSERT_EQ(prefix.size(), 2u);
    EXPECT_EQ(prefix[0].content, "one");
    EXPECT_EQ(prefix[1].role, "assistant");
    EXPECT_EQ(rewind_prefill_text(messages[2]), "two");
}

TEST(SessionRewind, TargetsAndPrefixAcrossToolMetaCompactAndShellPairs) {
    std::vector<ChatMessage> messages;
    messages.push_back(user("first"));
    messages.back().uuid = "u1";

    ChatMessage assistant;
    assistant.role = "assistant";
    assistant.content = "";
    assistant.tool_calls = nlohmann::json::array();
    messages.push_back(assistant);

    ChatMessage tool;
    tool.role = "tool";
    tool.content = "tool output";
    messages.push_back(tool);

    ChatMessage checkpoint;
    checkpoint.role = "system";
    checkpoint.is_meta = true;
    checkpoint.subtype = "file_checkpoint";
    messages.push_back(checkpoint);

    messages.push_back(user("!ls"));
    ChatMessage shell_result;
    shell_result.role = "tool_result";
    shell_result.content = "shell output";
    messages.push_back(shell_result);

    ChatMessage compact = user("compact summary");
    compact.is_compact_summary = true;
    messages.push_back(compact);

    messages.push_back(user("second"));
    messages.back().uuid = "u2";

    auto targets = collect_rewind_targets(messages);
    ASSERT_EQ(targets.size(), 2u);
    EXPECT_EQ(targets[0].message_uuid, "u1");
    EXPECT_EQ(targets[1].message_uuid, "u2");

    auto prefix = retained_prefix_before_index(messages, targets[1].message_index);
    ASSERT_EQ(prefix.size(), 7u);
    EXPECT_EQ(prefix[0].content, "first");
    EXPECT_EQ(prefix[3].subtype, "file_checkpoint");
    EXPECT_EQ(prefix[4].content, "!ls");
    EXPECT_TRUE(prefix[6].is_compact_summary);
}
