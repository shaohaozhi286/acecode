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
