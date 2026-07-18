#include <gtest/gtest.h>

#include "session/session_manager.hpp"
#include "session/session_auto_title.hpp"
#include "session/session_storage.hpp"
#include "session/session_title_generator.hpp"

#include <filesystem>
#include <random>

namespace {

std::filesystem::path temp_cwd(const std::string& hint) {
    auto dir = std::filesystem::temp_directory_path() /
        ("acecode_session_title_" + hint + "_" + std::to_string(std::random_device{}()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

struct ProjectCleanup {
    explicit ProjectCleanup(std::string cwd)
        : project_dir(acecode::SessionStorage::get_project_dir(cwd)) {}
    ~ProjectCleanup() { std::filesystem::remove_all(project_dir); }
    std::string project_dir;
};

class StaticTitleProvider final : public acecode::LlmProvider {
public:
    explicit StaticTitleProvider(acecode::ChatResponse response)
        : response_(std::move(response)) {}

    acecode::ChatResponse chat(
        const std::vector<acecode::ChatMessage>&,
        const std::vector<acecode::ToolDef>&) override {
        return response_;
    }

    void chat_stream(const std::vector<acecode::ChatMessage>&,
                     const std::vector<acecode::ToolDef>&,
                     const acecode::StreamCallback&,
                     std::atomic<bool>* = nullptr) override {}

    std::string name() const override { return "static-title"; }
    bool is_authenticated() override { return true; }
    std::string model() const override { return "static-title"; }
    void set_model(const std::string&) override {}

private:
    acecode::ChatResponse response_;
};

} // namespace

TEST(SessionTitle, GeneratedTitlePersistsWithGeneratedSource) {
    const auto cwd = temp_cwd("generated");
    ProjectCleanup cleanup(cwd.string());

    acecode::SessionManager sm;
    sm.start_session(cwd.string(), "test-provider", "test-model", "20260610-010203-abcd");

    ASSERT_EQ(sm.begin_auto_title_generation("fix auth timeout"),
              std::optional<std::string>{"fix auth timeout"});
    EXPECT_FALSE(sm.begin_auto_title_generation("ignored").has_value());
    ASSERT_TRUE(sm.try_set_generated_session_title("Fix auth timeout"));
    EXPECT_FALSE(sm.finish_auto_title_generation_for_session(
        "20260610-010203-abcd", true).has_value());
    EXPECT_EQ(sm.current_title(), "Fix auth timeout");
    EXPECT_EQ(sm.current_title_source(), "generated");

    ASSERT_EQ(sm.ensure_active_session_id(), "20260610-010203-abcd");
    const auto meta = acecode::SessionStorage::read_meta(
        acecode::SessionStorage::meta_path(cleanup.project_dir, "20260610-010203-abcd"));
    EXPECT_EQ(meta.title, "Fix auth timeout");
    EXPECT_EQ(meta.title_source, "generated");
}

TEST(SessionTitle, UserTitleBlocksGeneratedOverwrite) {
    const auto cwd = temp_cwd("user_wins");
    ProjectCleanup cleanup(cwd.string());

    acecode::SessionManager sm;
    sm.start_session(cwd.string(), "test-provider", "test-model", "20260610-020304-abcd");

    sm.set_session_title("Manual title");
    EXPECT_FALSE(sm.try_set_generated_session_title("Generated title"));
    EXPECT_EQ(sm.current_title(), "Manual title");
    EXPECT_EQ(sm.current_title_source(), "user");

    sm.set_session_title("");
    EXPECT_EQ(sm.current_title(), "");
    EXPECT_EQ(sm.current_title_source(), "");
    EXPECT_FALSE(sm.try_set_generated_session_title("Generated after clear"));
}

TEST(SessionTitle, GeneratedTitleForSessionRequiresActiveSessionMatch) {
    const auto cwd = temp_cwd("session_match");
    ProjectCleanup cleanup(cwd.string());

    acecode::SessionManager sm;
    sm.start_session(cwd.string(), "test-provider", "test-model", "20260610-020405-abcd");
    ASSERT_EQ(sm.ensure_active_session_id(), "20260610-020405-abcd");
    ASSERT_TRUE(sm.begin_auto_title_generation("matched session title"));

    EXPECT_FALSE(sm.try_set_generated_session_title_for_session(
        "other-session", "Wrong session title"));
    EXPECT_EQ(sm.current_title(), "");

    EXPECT_TRUE(sm.try_set_generated_session_title_for_session(
        "20260610-020405-abcd", "Matched session title"));
    EXPECT_EQ(sm.current_title(), "Matched session title");
    EXPECT_EQ(sm.current_title_source(), "generated");
}

TEST(SessionTitle, GeneratedErrorTitleIsRejectedAndRetriesAfterCompletedTurn) {
    const auto cwd = temp_cwd("generated_error_retry");
    ProjectCleanup cleanup(cwd.string());

    acecode::SessionManager sm;
    sm.start_session(cwd.string(), "test-provider", "test-model", "20260610-030405-abcd");

    ASSERT_EQ(sm.begin_auto_title_generation("fix the checkout retry path"),
              std::optional<std::string>{"fix the checkout retry path"});
    EXPECT_FALSE(sm.try_set_generated_session_title(
        "[Error] HTTP 402: quota exceeded"));
    EXPECT_TRUE(sm.current_title().empty());
    EXPECT_FALSE(sm.finish_auto_title_generation_for_session(
        "20260610-030405-abcd", false).has_value());

    auto retry = sm.mark_auto_title_turn_finished("completed");
    ASSERT_EQ(retry,
              std::optional<std::string>{"fix the checkout retry path"});
    ASSERT_TRUE(sm.try_set_generated_session_title("Fix checkout retry path"));
    EXPECT_FALSE(sm.finish_auto_title_generation_for_session(
        "20260610-030405-abcd", true).has_value());
    EXPECT_EQ(sm.current_title(), "Fix checkout retry path");
    EXPECT_EQ(sm.current_title_source(), "generated");
    EXPECT_FALSE(sm.begin_auto_title_generation("ignored").has_value());

    ASSERT_EQ(sm.ensure_active_session_id(), "20260610-030405-abcd");
    const auto meta = acecode::SessionStorage::read_meta(
        acecode::SessionStorage::meta_path(cleanup.project_dir, "20260610-030405-abcd"));
    EXPECT_EQ(meta.title, "Fix checkout retry path");
    EXPECT_EQ(meta.title_source, "generated");
}

TEST(SessionTitle, GeneratedNonErrorTitleDoesNotRefreshAfterFirstTurn) {
    const auto cwd = temp_cwd("generated_no_retry");
    ProjectCleanup cleanup(cwd.string());

    acecode::SessionManager sm;
    sm.start_session(cwd.string(), "test-provider", "test-model", "20260610-040506-abcd");

    ASSERT_TRUE(sm.begin_auto_title_generation("fix checkout retry path"));
    ASSERT_TRUE(sm.try_set_generated_session_title("Fix checkout retry path"));
    EXPECT_FALSE(sm.finish_auto_title_generation_for_session(
        "20260610-040506-abcd", true).has_value());

    acecode::ChatMessage user;
    user.role = "user";
    user.content = "now add tests";
    sm.on_message(user);

    EXPECT_FALSE(sm.begin_auto_title_generation("now add tests").has_value());
    EXPECT_EQ(sm.current_title(), "Fix checkout retry path");
}

TEST(SessionTitle, MainCompletionBeforeTitleFailureStartsOnlyOneRetry) {
    const auto cwd = temp_cwd("generated_error_late");
    ProjectCleanup cleanup(cwd.string());

    acecode::SessionManager sm;
    sm.start_session(cwd.string(), "test-provider", "test-model", "20260610-050607-abcd");

    ASSERT_TRUE(sm.begin_auto_title_generation("fix checkout retry path"));
    EXPECT_FALSE(sm.mark_auto_title_turn_finished("completed").has_value());

    auto retry = sm.finish_auto_title_generation_for_session(
        "20260610-050607-abcd", false);
    ASSERT_EQ(retry,
              std::optional<std::string>{"fix checkout retry path"});
    EXPECT_FALSE(sm.finish_auto_title_generation_for_session(
        "20260610-050607-abcd", false).has_value());
    EXPECT_FALSE(sm.mark_auto_title_turn_finished("completed").has_value());
    EXPECT_FALSE(sm.begin_auto_title_generation("ignored").has_value());
}

TEST(SessionTitle, FailedOrAbortedTurnDoesNotStartEventRetry) {
    const auto cwd = temp_cwd("generated_error_outcome");
    ProjectCleanup cleanup(cwd.string());

    acecode::SessionManager sm;
    sm.start_session(cwd.string(), "test-provider", "test-model", "20260610-060708-abcd");

    ASSERT_TRUE(sm.begin_auto_title_generation("fix checkout retry path"));
    EXPECT_FALSE(sm.finish_auto_title_generation_for_session(
        "20260610-060708-abcd", false).has_value());
    EXPECT_FALSE(sm.mark_auto_title_turn_finished("error").has_value());
    EXPECT_FALSE(sm.mark_auto_title_turn_finished("aborted").has_value());

    EXPECT_EQ(sm.begin_auto_title_generation("later visible input"),
              std::optional<std::string>{"fix checkout retry path"});
}

TEST(SessionTitle, ManualTitleCancelsPendingRetry) {
    const auto cwd = temp_cwd("generated_manual_wins");
    ProjectCleanup cleanup(cwd.string());

    acecode::SessionManager sm;
    sm.start_session(cwd.string(), "test-provider", "test-model", "20260610-070809-abcd");

    ASSERT_TRUE(sm.begin_auto_title_generation("fix checkout retry path"));
    EXPECT_FALSE(sm.finish_auto_title_generation_for_session(
        "20260610-070809-abcd", false).has_value());
    sm.set_session_title("Manual title");
    EXPECT_FALSE(sm.mark_auto_title_turn_finished("completed").has_value());
    EXPECT_FALSE(sm.begin_auto_title_generation("ignored").has_value());
    EXPECT_EQ(sm.current_title(), "Manual title");
}

TEST(SessionTitle, DelayedFailureDoesNotMutateReplacementSession) {
    const auto cwd = temp_cwd("generated_session_switch");
    ProjectCleanup cleanup(cwd.string());

    acecode::SessionManager sm;
    sm.start_session(cwd.string(), "test-provider", "test-model", "session-a");
    ASSERT_TRUE(sm.begin_auto_title_generation("session a input"));

    sm.start_session(cwd.string(), "test-provider", "test-model", "session-b");
    EXPECT_FALSE(sm.finish_auto_title_generation_for_session(
        "session-a", false).has_value());
    EXPECT_TRUE(sm.current_title().empty());
    EXPECT_EQ(sm.begin_auto_title_generation("session b input"),
              std::optional<std::string>{"session b input"});
}

TEST(SessionTitle, SanitizesJsonTitleOutput) {
    EXPECT_EQ(
        acecode::sanitize_generated_session_title(R"({"title":"  Refactor provider retry flow  "})"),
        "Refactor provider retry flow");
    EXPECT_EQ(acecode::sanitize_generated_session_title("Title:   Build web session title API"),
              "Build web session title API");
    EXPECT_FALSE(acecode::is_generated_session_error_title(
        "Error handling cleanup"));
    EXPECT_TRUE(acecode::is_generated_session_error_title(
        "  [Error] Connection failed"));
    EXPECT_TRUE(acecode::sanitize_generated_session_title(
        std::string("bad") + static_cast<char>(1) + "line").empty());
}

TEST(SessionTitle, GeneratorRejectsProviderErrorsAndToolCalls) {
    acecode::ChatResponse provider_error;
    provider_error.content = "[Error] Connection failed: timeout";
    provider_error.finish_reason = "error";
    StaticTitleProvider error_provider(provider_error);
    EXPECT_FALSE(acecode::generate_session_title(
        error_provider, "fix the title", 1000).has_value());

    acecode::ChatResponse tool_response;
    acecode::ToolCall call;
    call.id = "call-title";
    call.function_name = "bash";
    call.function_arguments = "{}";
    tool_response.tool_calls.push_back(std::move(call));
    StaticTitleProvider tool_provider(tool_response);
    EXPECT_FALSE(acecode::generate_session_title(
        tool_provider, "fix the title", 1000).has_value());

    acecode::ChatResponse valid_response;
    valid_response.content = R"({"title":"Error handling cleanup"})";
    valid_response.finish_reason = "stop";
    StaticTitleProvider valid_provider(valid_response);
    EXPECT_EQ(acecode::generate_session_title(
                  valid_provider, "fix the title", 1000),
              std::optional<std::string>{"Error handling cleanup"});
}

TEST(SessionTitle, VisibleAutoTitleInputPrefersDisplayText) {
    acecode::UserInput input;
    input.text = "raw model prompt";
    input.display_text = "  /plan user visible prompt  ";

    EXPECT_EQ(acecode::visible_auto_title_input(input), "/plan user visible prompt");
}

TEST(SessionTitle, VisibleAutoTitleInputFallsBackToTextContentParts) {
    acecode::UserInput input;
    input.content_parts = nlohmann::json::array({
        {{"type", "image_url"}, {"image_url", "ignored"}},
        {{"type", "text"}, {"text", "  first line  "}},
        {{"type", "text"}, {"text", "\nsecond line\n"}},
    });

    EXPECT_EQ(acecode::visible_auto_title_input(input), "first line\nsecond line");
}
