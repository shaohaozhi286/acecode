#include <gtest/gtest.h>

#include "cli/interactive_options.hpp"

#include <string>
#include <vector>

namespace {

acecode::InteractiveCliOptions parse_args(std::vector<std::string> args) {
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (auto& arg : args) {
        argv.push_back(arg.data());
    }
    return acecode::parse_interactive_cli_options(
        static_cast<int>(argv.size()), argv.data());
}

} // namespace

TEST(InteractiveOptions, ParsesQuickResumePickerFlag) {
    auto opts = parse_args({"acecode", "-r"});

    EXPECT_TRUE(opts.resume_picker_on_startup);
    EXPECT_FALSE(opts.direct_resume_requested());
}

TEST(InteractiveOptions, ResumeWithoutIdKeepsDirectLatestBehavior) {
    auto opts = parse_args({"acecode", "--resume"});

    EXPECT_TRUE(opts.resume_latest);
    EXPECT_TRUE(opts.direct_resume_requested());
    EXPECT_FALSE(opts.resume_picker_on_startup);
}

TEST(InteractiveOptions, ResumeWithIdKeepsDirectSpecificBehavior) {
    auto opts = parse_args({"acecode", "--resume", "session-123"});

    EXPECT_FALSE(opts.resume_latest);
    EXPECT_EQ(opts.resume_session_id, "session-123");
    EXPECT_TRUE(opts.direct_resume_requested());
}

TEST(InteractiveOptions, ExplicitResumeCanOverrideQuickPickerAtCallSite) {
    auto opts = parse_args({"acecode", "-r", "--resume", "session-123"});

    EXPECT_TRUE(opts.resume_picker_on_startup);
    EXPECT_EQ(opts.resume_session_id, "session-123");
    EXPECT_TRUE(opts.direct_resume_requested());
}

TEST(InteractiveOptions, ParsesExistingInteractiveFlags) {
    auto opts = parse_args({
        "acecode", "--yolo", "configure", "--validate-models-registry",
        "--alt-screen"
    });

    EXPECT_TRUE(opts.dangerous_mode);
    EXPECT_TRUE(opts.run_configure_cmd);
    EXPECT_TRUE(opts.validate_models_registry_cmd);
    EXPECT_TRUE(opts.force_alt_screen);
}

// ---- add-ask-question-policy: --question-policy ----

TEST(InteractiveOptions, QuestionPolicyAcceptsThreeStates) {
    for (const char* v : {"ask", "deny", "timeout"}) {
        auto opts = parse_args({"acecode", "--question-policy", v});
        EXPECT_EQ(opts.question_policy, v);
        EXPECT_EQ(opts.question_timeout_seconds, 0);
        EXPECT_TRUE(opts.question_policy_error.empty()) << v;
    }
}

TEST(InteractiveOptions, QuestionPolicyTimeoutColonSeconds) {
    auto opts = parse_args({"acecode", "--question-policy", "timeout:120"});
    EXPECT_EQ(opts.question_policy, "timeout");
    EXPECT_EQ(opts.question_timeout_seconds, 120);
    EXPECT_TRUE(opts.question_policy_error.empty());
}

TEST(InteractiveOptions, QuestionPolicyInvalidValueSetsError) {
    auto opts = parse_args({"acecode", "--question-policy", "sometimes"});
    EXPECT_TRUE(opts.question_policy.empty());
    EXPECT_FALSE(opts.question_policy_error.empty());
}

TEST(InteractiveOptions, QuestionPolicyMissingValueSetsError) {
    auto opts = parse_args({"acecode", "--question-policy"});
    EXPECT_TRUE(opts.question_policy.empty());
    EXPECT_FALSE(opts.question_policy_error.empty());
}

TEST(InteractiveOptions, QuestionPolicyTimeoutSecondsValidation) {
    // 越界(两侧)与非数字都必须报错
    for (const char* v : {"timeout:2", "timeout:4000", "timeout:", "timeout:abc",
                          "timeout:12x"}) {
        auto opts = parse_args({"acecode", "--question-policy", v});
        EXPECT_TRUE(opts.question_policy.empty()) << v;
        EXPECT_FALSE(opts.question_policy_error.empty()) << v;
    }
    // 边界值合法
    for (int secs : {5, 3600}) {
        auto opts = parse_args(
            {"acecode", "--question-policy", "timeout:" + std::to_string(secs)});
        EXPECT_EQ(opts.question_policy, "timeout");
        EXPECT_EQ(opts.question_timeout_seconds, secs);
        EXPECT_TRUE(opts.question_policy_error.empty());
    }
}

TEST(InteractiveOptions, QuestionPolicyCombinesWithYolo) {
    auto opts = parse_args({"acecode", "--yolo", "--question-policy", "ask"});
    EXPECT_TRUE(opts.dangerous_mode);
    EXPECT_EQ(opts.question_policy, "ask");
}
