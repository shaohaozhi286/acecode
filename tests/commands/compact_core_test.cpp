#include <gtest/gtest.h>

#include "commands/compact.hpp"

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
