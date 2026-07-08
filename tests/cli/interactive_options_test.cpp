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

// 场景:--worktree 不带名字(随机命名)与带名字两种用法,外加 -w 短参
// 和 --worktree=<name> 等号形式。
// 期望:worktree_enabled 置位;名字按各形式正确取值。
TEST(InteractiveOptions, ParsesWorktreeFlagForms) {
    auto bare = parse_args({"acecode", "--worktree"});
    EXPECT_TRUE(bare.worktree_enabled);
    EXPECT_TRUE(bare.worktree_name.empty());

    auto named = parse_args({"acecode", "--worktree", "feat-x"});
    EXPECT_TRUE(named.worktree_enabled);
    EXPECT_EQ(named.worktree_name, "feat-x");

    auto short_form = parse_args({"acecode", "-w", "feat-y"});
    EXPECT_TRUE(short_form.worktree_enabled);
    EXPECT_EQ(short_form.worktree_name, "feat-y");

    auto eq_form = parse_args({"acecode", "--worktree=feat-z"});
    EXPECT_TRUE(eq_form.worktree_enabled);
    EXPECT_EQ(eq_form.worktree_name, "feat-z");
}

// 场景:--worktree 后面跟的是另一个 flag(--yolo)而不是名字;以及
// PR 引用 "#123" 作为名字(以 '#' 开头,不能被误认成 flag 吞掉)。
// 期望:前者名字留空且 --yolo 正常生效;后者名字原样保留,交给启动
// 引导层解析成 pr-123。
TEST(InteractiveOptions, WorktreeValueDoesNotSwallowFlagsAndKeepsPrRefs) {
    auto with_flag = parse_args({"acecode", "--worktree", "--yolo"});
    EXPECT_TRUE(with_flag.worktree_enabled);
    EXPECT_TRUE(with_flag.worktree_name.empty());
    EXPECT_TRUE(with_flag.dangerous_mode);

    auto pr_ref = parse_args({"acecode", "--worktree", "#123"});
    EXPECT_TRUE(pr_ref.worktree_enabled);
    EXPECT_EQ(pr_ref.worktree_name, "#123");
}
