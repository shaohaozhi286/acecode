#include <gtest/gtest.h>

#include "headless/headless_options.hpp"

#include <string>
#include <vector>

using acecode::headless::parse_headless_cli_options;
using acecode::headless::should_enter_print_mode;

// ---- should_enter_print_mode ----

// 场景:命令行任意位置出现 -p 或 --print(prompt 在前 / flag 在前均可)。
// 期望:进入 print 模式判定为 true。
TEST(HeadlessShouldEnter, DetectsPrintFlagAnywhere) {
    EXPECT_TRUE(should_enter_print_mode({"-p", "hello"}));
    EXPECT_TRUE(should_enter_print_mode({"hello", "-p"}));
    EXPECT_TRUE(should_enter_print_mode({"--print"}));
    EXPECT_TRUE(should_enter_print_mode({"--yolo", "-p", "fix it"}));
}

// 场景:没有 -p / --print,或第一个 token 是已有子命令(configure/daemon/
// service/upgrade/update —— 它们各自有独立的参数空间,-p 可能是子命令自己
// 的参数,不能被无头模式劫持)。
// 期望:不进入 print 模式。
TEST(HeadlessShouldEnter, IgnoresSubcommandsAndPlainInvocations) {
    EXPECT_FALSE(should_enter_print_mode({}));
    EXPECT_FALSE(should_enter_print_mode({"--yolo"}));
    EXPECT_FALSE(should_enter_print_mode({"configure", "-p"}));
    EXPECT_FALSE(should_enter_print_mode({"daemon", "start", "-p"}));
    EXPECT_FALSE(should_enter_print_mode({"upgrade", "-p"}));
}

// ---- parse_headless_cli_options ----

// 场景:最常见用法 `acecode -p "prompt"`。
// 期望:print_mode 置位,prompt 取位置参数,其余保持默认。
TEST(HeadlessOptions, ParsesBasicPromptForm) {
    auto o = parse_headless_cli_options({"-p", "explain this repo"});
    EXPECT_TRUE(o.error.empty()) << o.error;
    EXPECT_TRUE(o.print_mode);
    EXPECT_EQ(o.prompt, "explain this repo");
    EXPECT_FALSE(o.dangerous_mode);
    EXPECT_EQ(o.max_turns, 0);
}

// 场景:prompt 在 -p 之前(`acecode "prompt" --print`),脚本作者两种顺序
// 都会写。
// 期望:解析结果与 -p 在前完全一致。
TEST(HeadlessOptions, PromptPositionIsOrderIndependent) {
    auto o = parse_headless_cli_options({"explain this repo", "--print"});
    EXPECT_TRUE(o.error.empty()) << o.error;
    EXPECT_TRUE(o.print_mode);
    EXPECT_EQ(o.prompt, "explain this repo");
}

// 场景:--yolo / --dangerous 及其单横线别名(与 TUI interactive_options
// 的别名集合保持一致,用户从 TUI 习惯迁移过来不该踩坑)。
// 期望:dangerous_mode 置位。
TEST(HeadlessOptions, ParsesDangerousAliases) {
    for (const char* alias : {"--yolo", "-yolo", "--dangerous", "-dangerous"}) {
        auto o = parse_headless_cli_options({"-p", alias, "do it"});
        EXPECT_TRUE(o.error.empty()) << alias << ": " << o.error;
        EXPECT_TRUE(o.dangerous_mode) << alias;
        EXPECT_EQ(o.prompt, "do it") << alias;
    }
}

// 场景:--permission-mode 空格形式与等号形式;四个合法值。
// 期望:原样透传;非法值报错(错误消息包含非法值本身,便于脚本排查)。
TEST(HeadlessOptions, ParsesAndValidatesPermissionMode) {
    for (const char* mode : {"default", "accept-edits", "plan", "yolo"}) {
        auto space = parse_headless_cli_options({"-p", "--permission-mode", mode, "x"});
        EXPECT_TRUE(space.error.empty()) << mode << ": " << space.error;
        EXPECT_EQ(space.permission_mode, mode);

        auto eq = parse_headless_cli_options(
            {"-p", std::string("--permission-mode=") + mode, "x"});
        EXPECT_TRUE(eq.error.empty()) << mode << ": " << eq.error;
        EXPECT_EQ(eq.permission_mode, mode);
    }

    auto bad = parse_headless_cli_options({"-p", "--permission-mode", "sudo", "x"});
    EXPECT_FALSE(bad.error.empty());
    EXPECT_NE(bad.error.find("sudo"), std::string::npos);
}

// 场景:--model 与 --max-turns 的空格 / 等号两种形式。
// 期望:值正确落位;--max-turns 只接受正整数。
TEST(HeadlessOptions, ParsesModelAndMaxTurns) {
    auto o = parse_headless_cli_options(
        {"-p", "--model", "copilot-fast", "--max-turns", "12", "go"});
    EXPECT_TRUE(o.error.empty()) << o.error;
    EXPECT_EQ(o.model_name, "copilot-fast");
    EXPECT_EQ(o.max_turns, 12);
    EXPECT_EQ(o.prompt, "go");

    auto eq = parse_headless_cli_options({"-p", "--model=m1", "--max-turns=3", "go"});
    EXPECT_TRUE(eq.error.empty()) << eq.error;
    EXPECT_EQ(eq.model_name, "m1");
    EXPECT_EQ(eq.max_turns, 3);
}

// 场景:--max-turns 传入 0 / 负数 / 非数字 / 带尾随字符(如 "10x")。
// 期望:全部报错 —— 0 与负数没有"跑 0 轮"的合理语义,静默钳制会掩盖脚本
// 的传参 bug。
TEST(HeadlessOptions, RejectsInvalidMaxTurns) {
    for (const char* bad : {"0", "-3", "abc", "10x", ""}) {
        auto o = parse_headless_cli_options(
            {"-p", std::string("--max-turns=") + bad, "go"});
        EXPECT_FALSE(o.error.empty()) << "value: '" << bad << "'";
    }
}

// 场景:缺少值的 flag(--model / --permission-mode / --max-turns 是最后
// 一个 token)。
// 期望:明确报"requires"错误而不是越界或吞掉。
TEST(HeadlessOptions, RejectsFlagsMissingValues) {
    for (const char* flag : {"--model", "--permission-mode", "--max-turns"}) {
        auto o = parse_headless_cli_options({"-p", flag});
        EXPECT_FALSE(o.error.empty()) << flag;
        EXPECT_NE(o.error.find("requires"), std::string::npos) << o.error;
    }
}

// 场景:拼错的 flag(--modle)。-p 模式常被 CI 脚本调用,静默吞掉未知
// flag 会把拼写错误变成难查的行为差异。
// 期望:硬报错,错误消息带上原 token。
TEST(HeadlessOptions, RejectsUnknownFlags) {
    auto o = parse_headless_cli_options({"-p", "--modle", "m1", "go"});
    EXPECT_FALSE(o.error.empty());
    EXPECT_NE(o.error.find("--modle"), std::string::npos);
}

// 场景:两个位置参数(用户忘了给 prompt 加引号,shell 把它拆成了多个
// token)。
// 期望:报错并提示"把 prompt 引成单个参数",这是该场景下最常见的修法。
TEST(HeadlessOptions, RejectsMultiplePositionalArguments) {
    auto o = parse_headless_cli_options({"-p", "fix", "the", "bug"});
    EXPECT_FALSE(o.error.empty());
    EXPECT_NE(o.error.find("quote"), std::string::npos);
}

// 场景:只有 -p 没有 prompt(合法 —— prompt 可以来自 stdin 管道,由
// runner 层再判空)。
// 期望:解析通过,prompt 为空串。
TEST(HeadlessOptions, AllowsEmptyPromptForStdinPiping) {
    auto o = parse_headless_cli_options({"-p"});
    EXPECT_TRUE(o.error.empty()) << o.error;
    EXPECT_TRUE(o.print_mode);
    EXPECT_TRUE(o.prompt.empty());
}
