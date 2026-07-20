#include <gtest/gtest.h>

#include "commands/compact.hpp"
#include "commands/micro_compact.hpp"

namespace {

class ChatStubProvider : public acecode::LlmProvider {
public:
    acecode::ChatResponse chat(
        const std::vector<acecode::ChatMessage>& messages,
        const std::vector<acecode::ToolDef>&) override {
        last_messages = messages;
        acecode::ChatResponse resp;
        resp.content = response_content;
        resp.finish_reason = finish_reason;
        return resp;
    }

    void chat_stream(const std::vector<acecode::ChatMessage>&,
                     const std::vector<acecode::ToolDef>&,
                     const acecode::StreamCallback&,
                     std::atomic<bool>* = nullptr) override {}

    std::string name() const override { return "stub"; }
    bool is_authenticated() override { return true; }
    std::string model() const override { return "stub"; }
    void set_model(const std::string&) override {}

    std::vector<acecode::ChatMessage> last_messages;
    std::string response_content = "<summary>Important retained context.</summary>";
    std::string finish_reason = "stop";
};

acecode::ChatMessage msg(std::string role, std::string content, std::string uuid = {}) {
    acecode::ChatMessage out;
    out.role = std::move(role);
    out.content = std::move(content);
    out.uuid = std::move(uuid);
    return out;
}

std::string long_text(char c) {
    return std::string(900, c);
}

} // namespace

TEST(CompactCore, SuccessfulCompactionReturnsReplacementMessages) {
    ChatStubProvider provider;
    std::vector<acecode::ChatMessage> messages{
        msg("user", long_text('a'), "u-old"),
        msg("assistant", long_text('b')),
        msg("user", "keep this", "u-keep"),
        msg("assistant", "kept response"),
    };

    auto result = acecode::compact_messages(provider, messages, "/tmp/project", 1);

    ASSERT_TRUE(result.performed) << result.error;
    EXPECT_EQ(result.messages_compressed, 2);
    EXPECT_GT(result.estimated_tokens_saved, 0);
    ASSERT_GE(result.compacted_messages.size(), 5u);
    EXPECT_EQ(result.compacted_messages[0].subtype, "compact_boundary");
    EXPECT_TRUE(result.compacted_messages[1].is_compact_summary);
    EXPECT_NE(result.compacted_messages[1].content.find("Important retained context"), std::string::npos);
    EXPECT_TRUE(result.compacted_messages[2].is_meta);
    EXPECT_NE(result.compacted_messages[2].content.find("/tmp/project"), std::string::npos);
    EXPECT_EQ(result.compacted_messages[3].content, "keep this");
    EXPECT_EQ(provider.last_messages.size(), 2u);
}

TEST(CompactCore, InsufficientHistoryDoesNotCallProvider) {
    ChatStubProvider provider;
    std::vector<acecode::ChatMessage> messages{
        msg("user", "short", "u1"),
        msg("assistant", "short"),
    };

    auto result = acecode::compact_messages(provider, messages, "/tmp/project", 4);

    EXPECT_FALSE(result.performed);
    EXPECT_EQ(result.error, "Not enough conversation history to compact.");
    EXPECT_TRUE(provider.last_messages.empty());
}

TEST(CompactCore, ProviderFailureReturnsError) {
    ChatStubProvider provider;
    provider.finish_reason = "error";
    provider.response_content = "provider unavailable";
    std::vector<acecode::ChatMessage> messages{
        msg("user", long_text('a'), "u-old"),
        msg("assistant", long_text('b')),
        msg("user", "keep this", "u-keep"),
        msg("assistant", "kept response"),
    };

    auto result = acecode::compact_messages(provider, messages, "/tmp/project", 1);

    EXPECT_FALSE(result.performed);
    EXPECT_EQ(result.error, "Summarization failed: provider unavailable");
    EXPECT_TRUE(result.compacted_messages.empty());
}

TEST(CompactCore, AutoCompactTriggersWhenProviderHistoryExceedsMessageLimit) {
    std::vector<acecode::ChatMessage> messages;
    for (int i = 0; i <= acecode::AUTOCOMPACT_MAX_PROVIDER_MESSAGES; ++i) {
        messages.push_back(msg("user", "x"));
    }

    EXPECT_TRUE(acecode::should_auto_compact(messages, 1000000, 1));
}

TEST(CompactCore, AutoCompactStillTriggersFromTokensWithinMessageLimit) {
    std::vector<acecode::ChatMessage> messages{
        msg("user", "small history"),
    };
    const int context_window = 128000;
    const int threshold = acecode::get_auto_compact_threshold(context_window);

    EXPECT_TRUE(acecode::should_auto_compact(
        messages, context_window, threshold + 1));
}

TEST(CompactCore, AutoCompactStaysOffWhenTokensAndProviderHistoryAreWithinBounds) {
    std::vector<acecode::ChatMessage> messages;
    for (int i = 0; i < acecode::AUTOCOMPACT_MAX_PROVIDER_MESSAGES; ++i) {
        messages.push_back(msg("assistant", "ok"));
    }
    auto transcript_only = msg("system", "not sent to provider");
    transcript_only.metadata = {{"transcript_only", true}};
    messages.push_back(std::move(transcript_only));

    EXPECT_FALSE(acecode::should_auto_compact(messages, 1000000, 1));
}

TEST(CompactCore, ContextOverflowClassificationHandlesCommonProviderShapes) {
    acecode::ProviderErrorInfo explicit_code;
    explicit_code.kind = acecode::ProviderErrorKind::Http;
    explicit_code.status_code = 400;
    explicit_code.raw_body = R"({"error":{"code":"context_length_exceeded"}})";
    EXPECT_TRUE(acecode::is_context_overflow_error(explicit_code));
    EXPECT_TRUE(acecode::should_attempt_context_overflow_rescue(
        explicit_code, 1000, 128000, false));

    acecode::ProviderErrorInfo payload_too_large;
    payload_too_large.kind = acecode::ProviderErrorKind::Http;
    payload_too_large.status_code = 413;
    EXPECT_TRUE(acecode::is_context_overflow_error(payload_too_large));

    acecode::ProviderErrorInfo ambiguous_large_request;
    ambiguous_large_request.kind = acecode::ProviderErrorKind::Http;
    ambiguous_large_request.status_code = 400;
    ambiguous_large_request.display_message = "Bad Request";
    EXPECT_TRUE(acecode::should_attempt_context_overflow_rescue(
        ambiguous_large_request, 64000, 64000, false));
    EXPECT_TRUE(acecode::should_attempt_context_overflow_rescue(
        ambiguous_large_request, 64000, 200000, false));
    EXPECT_FALSE(acecode::should_attempt_context_overflow_rescue(
        ambiguous_large_request, 1000, 200000, false));

    acecode::ProviderErrorInfo network;
    network.kind = acecode::ProviderErrorKind::Network;
    network.display_message = "connection reset";
    EXPECT_FALSE(acecode::should_attempt_context_overflow_rescue(
        network, 64000, 64000, false));
    EXPECT_FALSE(acecode::should_attempt_context_overflow_rescue(
        explicit_code, 64000, 64000, true));
}

TEST(CompactCore, RescueCompactPreservesRecentTailWithoutProviderCall) {
    std::vector<acecode::ChatMessage> messages{
        msg("user", "old user 0 " + long_text('a'), "u-old-0"),
        msg("assistant", "old assistant 0 " + long_text('b')),
        msg("user", "old user 1 " + long_text('c'), "u-old-1"),
        msg("assistant", "old assistant 1 " + long_text('d')),
        msg("user", "latest user", "u-latest"),
        msg("assistant", "latest assistant"),
    };

    auto result = acecode::rescue_compact_messages(messages, "/tmp/project", 1);

    ASSERT_TRUE(result.performed) << result.error;
    EXPECT_TRUE(result.can_retry);
    EXPECT_EQ(result.messages_removed, 4);
    EXPECT_EQ(result.protected_user_turns, 1);
    EXPECT_LT(result.estimated_tokens_after, result.estimated_tokens_before);
    ASSERT_GE(result.compacted_messages.size(), 5u);
    EXPECT_EQ(result.compacted_messages[0].subtype, "compact_boundary");
    EXPECT_TRUE(result.compacted_messages[1].is_compact_summary);
    EXPECT_NE(result.compacted_messages[1].content.find("detailed summary was not generated"),
              std::string::npos);
    EXPECT_TRUE(result.compacted_messages[2].is_meta);
    EXPECT_NE(result.compacted_messages[2].content.find("/tmp/project"), std::string::npos);
    EXPECT_EQ(result.compacted_messages[3].content, "latest user");
    EXPECT_EQ(result.compacted_messages[4].content, "latest assistant");
}

TEST(CompactCore, RescueCompactReturnsClearErrorWhenSingleTurnCannotShrink) {
    std::vector<acecode::ChatMessage> messages{
        msg("user", long_text('x'), "u-only"),
    };

    auto result = acecode::rescue_compact_messages(messages, "/tmp/project", 4);

    EXPECT_FALSE(result.performed);
    EXPECT_FALSE(result.can_retry);
    EXPECT_TRUE(result.compacted_messages.empty());
    EXPECT_NE(result.error.find("too large"), std::string::npos);
    EXPECT_EQ(result.estimated_tokens_after, result.estimated_tokens_before);
}

TEST(CompactCore, CountableUserTurnIgnoresHiddenGoalContext) {
    auto real = msg("user", "real question");
    auto goal = msg("user", "<goal_context>continue</goal_context>");
    goal.metadata = {{"hidden_goal_context", true}};
    auto todo = msg("user", "<todo_context>");
    todo.metadata = {{"hidden_todo_context", true}};

    EXPECT_TRUE(acecode::is_countable_user_turn(real));
    EXPECT_FALSE(acecode::is_countable_user_turn(goal));
    EXPECT_FALSE(acecode::is_countable_user_turn(todo));
}

TEST(CompactCore, CompactKeepTurnsSkipsHiddenGoalContext) {
    ChatStubProvider provider;
    std::vector<acecode::ChatMessage> messages{
        msg("user", long_text('a'), "u-old"),
        msg("assistant", long_text('b')),
        msg("user", "keep this real user", "u-keep"),
        msg("assistant", "kept response"),
    };
    auto goal = msg("user", "<goal_context>continue work</goal_context>", "u-goal");
    goal.metadata = {{"hidden_goal_context", true}};
    messages.push_back(std::move(goal));
    messages.push_back(msg("assistant", "goal turn response"));

    // keep_turns=1 must retain the real user turn, not treat goal injection as
    // the only protected user message (which would drop "keep this real user").
    auto result = acecode::compact_messages(provider, messages, "/tmp/project", 1);

    ASSERT_TRUE(result.performed) << result.error;
    bool saw_real_keep = false;
    bool saw_goal = false;
    for (const auto& m : result.compacted_messages) {
        if (m.content == "keep this real user") saw_real_keep = true;
        if (m.metadata.is_object() &&
            m.metadata.value("hidden_goal_context", false)) {
            saw_goal = true;
        }
    }
    EXPECT_TRUE(saw_real_keep);
    EXPECT_TRUE(saw_goal);
}

acecode::ChatMessage tool_call(const std::string& id, const std::string& name) {
    acecode::ChatMessage out;
    out.role = "assistant";
    out.content = "";
    out.tool_calls = nlohmann::json::array({
        {
            {"id", id},
            {"type", "function"},
            {"function", {{"name", name}, {"arguments", "{}"}}},
        },
    });
    return out;
}

acecode::ChatMessage tool_result(const std::string& id, const std::string& content) {
    acecode::ChatMessage out;
    out.role = "tool";
    out.tool_call_id = id;
    out.content = content;
    return out;
}

TEST(MicroCompact, ProtectsRecentUserTurnToolResults) {
    std::vector<acecode::ChatMessage> messages{
        msg("user", "old"),
        tool_call("c-old", "file_read"),
        tool_result("c-old", std::string(2000, 'O')),
        msg("user", "recent"),
        tool_call("c-new", "file_read"),
        tool_result("c-new", std::string(2000, 'N')),
    };

    auto result = acecode::run_micro_compact(messages, 0, 1);

    ASSERT_TRUE(result.performed);
    EXPECT_EQ(result.tool_results_cleared, 1);
    EXPECT_EQ(messages[2].content, "[Older tool output omitted from context]");
    EXPECT_EQ(messages[5].content, std::string(2000, 'N'));
}

TEST(MicroCompact, NeverClearsFileEditOrWriteResults) {
    std::vector<acecode::ChatMessage> messages{
        msg("user", "old edit"),
        tool_call("c-edit", "file_edit"),
        tool_result("c-edit", std::string(2000, 'E')),
        tool_call("c-write", "file_write"),
        tool_result("c-write", std::string(2000, 'W')),
        msg("user", "later"),
        msg("assistant", "ok"),
        msg("user", "later2"),
        msg("assistant", "ok2"),
        msg("user", "later3"),
        msg("assistant", "ok3"),
        msg("user", "later4"),
        msg("assistant", "ok4"),
    };

    auto result = acecode::run_micro_compact(messages, 0, 4);

    EXPECT_FALSE(result.performed);
    EXPECT_EQ(messages[2].content, std::string(2000, 'E'));
    EXPECT_EQ(messages[4].content, std::string(2000, 'W'));
}

TEST(MicroCompact, ProtectsEverythingWhenFewerThanKeepUserTurns) {
    // Previously, not reaching keep_assistant_turns left cutoff at end and
    // wiped *all* tool results. With insufficient real user turns we must
    // clear nothing.
    std::vector<acecode::ChatMessage> messages{
        msg("user", "only one real turn"),
        tool_call("c1", "bash"),
        tool_result("c1", std::string(2000, 'x')),
        msg("assistant", "done"),
    };
    auto goal = msg("user", "<goal_context>continue</goal_context>");
    goal.metadata = {{"hidden_goal_context", true}};
    messages.push_back(std::move(goal));
    messages.push_back(tool_call("c2", "bash"));
    messages.push_back(tool_result("c2", std::string(2000, 'y')));

    auto result = acecode::run_micro_compact(messages, 0, 4);

    EXPECT_FALSE(result.performed);
    EXPECT_EQ(messages[2].content, std::string(2000, 'x'));
    EXPECT_EQ(messages[6].content, std::string(2000, 'y'));
}
