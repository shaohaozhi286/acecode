#include <gtest/gtest.h>

#include "commands/compact.hpp"
#include "commands/compact_prompt.hpp"

#include <deque>
#include <stdexcept>

namespace {

class ChatStubProvider : public acecode::LlmProvider {
public:
    acecode::ChatResponse chat(
        const std::vector<acecode::ChatMessage>& messages,
        const std::vector<acecode::ToolDef>&) override {
        calls.push_back(messages);
        if (!exceptions.empty()) {
            const std::string error = exceptions.front();
            exceptions.pop_front();
            throw std::runtime_error(error);
        }
        if (!responses.empty()) {
            auto response = responses.front();
            responses.pop_front();
            return response;
        }
        acecode::ChatResponse response;
        response.content = "Important retained context.";
        response.finish_reason = "stop";
        return response;
    }

    void chat_stream(const std::vector<acecode::ChatMessage>&,
                     const std::vector<acecode::ToolDef>&,
                     const acecode::StreamCallback&,
                     std::atomic<bool>* = nullptr) override {}

    std::string name() const override { return "stub"; }
    bool is_authenticated() override { return true; }
    std::string model() const override { return "stub"; }
    void set_model(const std::string&) override {}
    bool supports_native_compaction() const override {
        return native_capability;
    }

    static acecode::ChatResponse response(std::string content,
                                          std::string finish_reason = "stop") {
        acecode::ChatResponse out;
        out.content = std::move(content);
        out.finish_reason = std::move(finish_reason);
        return out;
    }

    std::vector<std::vector<acecode::ChatMessage>> calls;
    std::deque<acecode::ChatResponse> responses;
    std::deque<std::string> exceptions;
    bool native_capability = false;
};

acecode::ChatMessage msg(std::string role,
                         std::string content,
                         std::string uuid = {}) {
    acecode::ChatMessage out;
    out.role = std::move(role);
    out.content = std::move(content);
    out.uuid = std::move(uuid);
    return out;
}

acecode::ChatMessage tool_call_message(const std::string& id) {
    auto out = msg("assistant", "");
    out.tool_calls = nlohmann::json::array({
        {
            {"id", id},
            {"type", "function"},
            {"function", {{"name", "probe"}, {"arguments", "{}"}}},
        },
    });
    return out;
}

acecode::ChatMessage tool_output_message(const std::string& id) {
    auto out = msg("tool", "probe output");
    out.tool_call_id = id;
    return out;
}

} // namespace

TEST(CompactCore, UsesExactCodexPromptAndSummaryShape) {
    ChatStubProvider provider;
    std::vector<acecode::ChatMessage> initial_context{
        msg("system", "stable base instructions"),
    };
    std::vector<acecode::ChatMessage> messages{
        msg("user", "first request", "u1"),
        msg("assistant", "first answer"),
        msg("tool", "tool output"),
        msg("user", "latest request", "u2"),
    };

    auto result = acecode::compact_messages(
        provider, messages, initial_context, false, nullptr);

    ASSERT_TRUE(result.performed) << result.error;
    ASSERT_EQ(provider.calls.size(), 1u);
    const auto& request = provider.calls.front();
    ASSERT_EQ(request.size(), initial_context.size() + messages.size() + 1);
    EXPECT_EQ(request.front().content, "stable base instructions");
    EXPECT_EQ(request.back().role, "user");
    EXPECT_EQ(request.back().content, acecode::get_compact_prompt());
    EXPECT_EQ(request[2].role, "assistant");
    EXPECT_EQ(request[3].role, "tool");

    ASSERT_EQ(result.compacted_messages.size(), 3u);
    EXPECT_EQ(result.compacted_messages[0].content, "first request");
    EXPECT_EQ(result.compacted_messages[1].content, "latest request");
    EXPECT_EQ(result.compacted_messages[2].role, "user");
    EXPECT_TRUE(result.compacted_messages[2].is_compact_summary);
    EXPECT_EQ(
        result.compacted_messages[2].content,
        acecode::get_compact_summary_prefix() +
            "\nImportant retained context.");
    EXPECT_EQ(result.summary_text, "Important retained context.");
}

TEST(CompactCore, EmptyHistoryStillRunsCheckpointPrompt) {
    ChatStubProvider provider;

    auto result = acecode::compact_messages(provider, {});

    ASSERT_TRUE(result.performed) << result.error;
    ASSERT_EQ(provider.calls.size(), 1u);
    ASSERT_EQ(provider.calls[0].size(), 1u);
    EXPECT_EQ(provider.calls[0][0].content, acecode::get_compact_prompt());
    ASSERT_EQ(result.compacted_messages.size(), 1u);
    EXPECT_EQ(result.compacted_messages[0].content,
              acecode::get_compact_summary_prefix() +
                  "\nImportant retained context.");
}

TEST(CompactCore, IncompleteNativeCapabilityFallsBackToValidatedLocalPath) {
    ChatStubProvider provider;
    provider.native_capability = true;

    auto result = acecode::compact_messages(
        provider, {msg("user", "keep this request")});

    ASSERT_TRUE(result.performed) << result.error;
    ASSERT_EQ(provider.calls.size(), 1u);
    EXPECT_EQ(provider.calls[0].back().content, acecode::get_compact_prompt());
    EXPECT_EQ(result.compacted_messages.back().content,
              acecode::get_compact_summary_prefix() +
                  "\nImportant retained context.");
}

TEST(CompactCore, RetainsNewestUserTextWithinTwentyThousandTokenBudget) {
    const std::string old_user(40000, 'a');   // 10,000 approximate tokens
    const std::string new_user(60000, 'b');   // 15,000 approximate tokens
    std::vector<acecode::ChatMessage> messages{
        msg("user", old_user, "old"),
        msg("assistant", "answer"),
        msg("user", new_user, "new"),
    };

    auto compacted = acecode::build_compacted_history(messages, "summary");

    ASSERT_EQ(compacted.size(), 3u);
    EXPECT_EQ(compacted[0].uuid, "old");
    EXPECT_NE(compacted[0].content.find("5000 tokens truncated"),
              std::string::npos);
    EXPECT_EQ(compacted[0].content.substr(0, 32), std::string(32, 'a'));
    EXPECT_EQ(compacted[0].content.substr(compacted[0].content.size() - 32),
              std::string(32, 'a'));
    EXPECT_EQ(compacted[1].uuid, "new");
    EXPECT_EQ(compacted[1].content, new_user);
    EXPECT_EQ(compacted[2].content,
              acecode::get_compact_summary_prefix() + "\nsummary");
}

TEST(CompactCore, ExcludesPriorSummaryAndNonUserItems) {
    auto previous_summary = msg(
        "user",
        acecode::get_compact_summary_prefix() + "\nold summary");
    previous_summary.is_compact_summary = true;
    auto structured_user = msg("user", "real request", "real");
    structured_user.content_parts = nlohmann::json::array({
        {{"type", "text"}, {"text", "real request"}},
        {{"type", "image_url"}, {"image_url", "data:image/png;base64,abc"}},
    });
    std::vector<acecode::ChatMessage> messages{
        std::move(previous_summary),
        msg("assistant", "assistant detail"),
        msg("tool", "tool detail"),
        std::move(structured_user),
    };

    auto compacted = acecode::build_compacted_history(messages, "new summary");

    ASSERT_EQ(compacted.size(), 2u);
    EXPECT_EQ(compacted[0].content, "real request");
    EXPECT_TRUE(compacted[0].content_parts.empty());
    EXPECT_EQ(compacted[1].content,
              acecode::get_compact_summary_prefix() + "\nnew summary");
}

TEST(CompactCore, ExcludesInternalUserContextRows) {
    auto internal = [](const char* key, const char* content) {
        auto message = msg("user", content);
        message.metadata = nlohmann::json{{key, true}};
        return message;
    };
    std::vector<acecode::ChatMessage> messages{
        msg("user", "real request", "real"),
        internal("hidden_goal_context", "goal steering"),
        internal("hidden_plan_mode_context", "plan instructions"),
        internal("hidden_todo_context", "todo injection"),
        internal("hidden_hook_stop_continuation", "hook continuation"),
        internal("compact_initial_context", "rebuilt session context"),
        internal("transcript_only", "human transcript marker"),
    };

    auto compacted = acecode::build_compacted_history(messages, "summary");

    ASSERT_EQ(compacted.size(), 2u);
    EXPECT_EQ(compacted[0].uuid, "real");
    EXPECT_EQ(compacted[0].content, "real request");
    EXPECT_EQ(compacted[1].content,
              acecode::get_compact_summary_prefix() + "\nsummary");
}

TEST(CompactCore, ContextOverflowRemovesOneOldestHistoryItemPerRetry) {
    ChatStubProvider provider;
    provider.responses.push_back(ChatStubProvider::response(
        "maximum context length exceeded", "error"));
    provider.responses.push_back(ChatStubProvider::response("summary"));
    std::vector<acecode::ChatMessage> initial_context{
        msg("system", "stable"),
    };
    std::vector<acecode::ChatMessage> messages{
        msg("user", "oldest"),
        msg("assistant", "middle"),
        msg("user", "newest"),
    };

    auto result = acecode::compact_messages(
        provider, messages, initial_context, true, nullptr);

    ASSERT_TRUE(result.performed) << result.error;
    ASSERT_EQ(provider.calls.size(), 2u);
    EXPECT_EQ(provider.calls[0].size(), 5u);
    EXPECT_EQ(provider.calls[1].size(), 4u);
    EXPECT_EQ(provider.calls[0][0].content, "stable");
    EXPECT_EQ(provider.calls[1][0].content, "stable");
    EXPECT_EQ(provider.calls[0].back().content, acecode::get_compact_prompt());
    EXPECT_EQ(provider.calls[1].back().content, acecode::get_compact_prompt());
    EXPECT_EQ(provider.calls[0][1].content, "oldest");
    EXPECT_EQ(provider.calls[1][1].content, "middle");
    EXPECT_EQ(result.compaction_request_items_removed, 1);
}

TEST(CompactCore, OverflowWithOnlyPromptFailsWithoutReplacement) {
    ChatStubProvider provider;
    provider.responses.push_back(ChatStubProvider::response(
        "context_length_exceeded", "error"));

    auto result = acecode::compact_messages(provider, {});

    EXPECT_FALSE(result.performed);
    EXPECT_TRUE(result.compacted_messages.empty());
    EXPECT_NE(result.error.find("no removable history item"), std::string::npos);
}

TEST(CompactCore, ContextOverflowExceptionUsesSameOneItemRetry) {
    ChatStubProvider provider;
    provider.exceptions.push_back("prompt is too long");
    provider.responses.push_back(ChatStubProvider::response("summary"));
    std::vector<acecode::ChatMessage> messages{
        msg("user", "oldest"),
        msg("assistant", "newest"),
    };

    auto result = acecode::compact_messages(provider, messages);

    ASSERT_TRUE(result.performed) << result.error;
    ASSERT_EQ(provider.calls.size(), 2u);
    EXPECT_EQ(provider.calls[0].size(), 3u);
    EXPECT_EQ(provider.calls[1].size(), 2u);
    EXPECT_EQ(provider.calls[1][0].content, "newest");
}

TEST(CompactCore, OverflowRetryRemovesMatchingToolOutputWithOldestCall) {
    ChatStubProvider provider;
    provider.responses.push_back(ChatStubProvider::response(
        "maximum context length exceeded", "error"));
    provider.responses.push_back(ChatStubProvider::response(
        "maximum context length exceeded", "error"));
    provider.responses.push_back(ChatStubProvider::response("summary"));
    std::vector<acecode::ChatMessage> messages{
        msg("user", "old turn"),
        tool_call_message("call-old"),
        tool_output_message("call-old"),
        msg("user", "new turn"),
    };

    auto result = acecode::compact_messages(provider, messages);

    ASSERT_TRUE(result.performed) << result.error;
    ASSERT_EQ(provider.calls.size(), 3u);
    EXPECT_EQ(provider.calls[0].size(), 5u);
    EXPECT_EQ(provider.calls[1].size(), 4u);
    ASSERT_EQ(provider.calls[2].size(), 2u);
    EXPECT_EQ(provider.calls[2][0].content, "new turn");
    EXPECT_EQ(provider.calls[2][1].content, acecode::get_compact_prompt());
    EXPECT_EQ(result.compaction_request_items_removed, 3);
}

TEST(CompactCore, TerminalFailureDoesNotInstallHistory) {
    ChatStubProvider provider;
    provider.responses.push_back(
        ChatStubProvider::response("provider unavailable", "error"));
    std::vector<acecode::ChatMessage> messages{msg("user", "request")};

    auto result = acecode::compact_messages(provider, messages);

    EXPECT_FALSE(result.performed);
    EXPECT_EQ(result.error, "Summarization failed: provider unavailable");
    EXPECT_TRUE(result.compacted_messages.empty());
}

TEST(CompactCore, UsesCodexByteTokenEstimateAndUtf8SafeTruncation) {
    EXPECT_EQ(acecode::approx_token_count(""), 0u);
    EXPECT_EQ(acecode::approx_token_count("a"), 1u);
    EXPECT_EQ(acecode::approx_token_count("abcd"), 1u);
    EXPECT_EQ(acecode::approx_token_count("abcde"), 2u);

    const std::string chinese = u8"甲乙丙丁戊己庚辛壬癸";
    const std::string truncated =
        acecode::truncate_text_to_token_budget(chinese, 2);
    EXPECT_NE(truncated.find("tokens truncated"), std::string::npos);
    EXPECT_NO_THROW({
        nlohmann::json value = truncated;
        (void)value.dump();
    });
}

TEST(CompactCore, AutomaticThresholdsMatchCodexPercentages) {
    EXPECT_EQ(acecode::get_effective_context_window(100000), 95000);
    EXPECT_EQ(acecode::get_auto_compact_threshold(100000), 90000);
    EXPECT_FALSE(acecode::should_auto_compact(100000, 89999, 100));
    EXPECT_TRUE(acecode::should_auto_compact(100000, 90000, 100));
    EXPECT_TRUE(acecode::should_auto_compact(100000, 100, 90000));
}

TEST(CompactCore, ContextOverflowClassificationHandlesProviderShapes) {
    acecode::ProviderErrorInfo explicit_code;
    explicit_code.kind = acecode::ProviderErrorKind::Http;
    explicit_code.status_code = 400;
    explicit_code.raw_body =
        R"({"error":{"code":"context_length_exceeded"}})";
    EXPECT_TRUE(acecode::is_context_overflow_error(explicit_code));

    acecode::ProviderErrorInfo payload_too_large;
    payload_too_large.kind = acecode::ProviderErrorKind::Http;
    payload_too_large.status_code = 413;
    EXPECT_TRUE(acecode::is_context_overflow_error(payload_too_large));

    acecode::ProviderErrorInfo network;
    network.kind = acecode::ProviderErrorKind::Network;
    network.display_message = "connection reset";
    EXPECT_FALSE(acecode::is_context_overflow_error(network));
}
