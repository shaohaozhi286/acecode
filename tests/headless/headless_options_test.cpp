#include <gtest/gtest.h>

#include "headless/headless_name_selection.hpp"
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
    EXPECT_TRUE(o.disabled_system_tools.empty());
    EXPECT_TRUE(o.enabled_skills.empty());
    EXPECT_TRUE(o.enabled_mcp_servers.empty());
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
    for (const char* flag : {"--model", "--permission-mode", "--max-turns",
                             "--disable-tools", "--enable-skills", "--enable-mcp"}) {
        auto o = parse_headless_cli_options({"-p", flag});
        EXPECT_FALSE(o.error.empty()) << flag;
        EXPECT_NE(o.error.find("requires"), std::string::npos) << o.error;
    }
}

// 场景:三个 capability list 都支持空格/等号、逗号列表和重复 flag;名称
// 两端空白被裁掉,重复项保留第一次出现的位置。
// 期望:得到稳定、去重且仍区分大小写的精确名称列表。
TEST(HeadlessOptions, ParsesRepeatableCapabilityLists) {
    auto o = parse_headless_cli_options({
        "-p",
        "--disable-tools", "bash, file_write",
        "--disable-tools=grep,bash",
        "--enable-skills=code-review, docs",
        "--enable-skills", "code-review",
        "--enable-mcp", "github",
        "--enable-mcp=linear, github",
        "go",
    });
    EXPECT_TRUE(o.error.empty()) << o.error;
    EXPECT_EQ(o.disabled_system_tools,
              (std::vector<std::string>{"bash", "file_write", "grep"}));
    EXPECT_EQ(o.enabled_skills,
              (std::vector<std::string>{"code-review", "docs"}));
    EXPECT_EQ(o.enabled_mcp_servers,
              (std::vector<std::string>{"github", "linear"}));
    EXPECT_EQ(o.prompt, "go");
}

// 场景:空 value、首尾逗号与连续逗号都会产生空名称。
// 期望:parser 层直接报用法错误,不把半截列表留给 runner。
TEST(HeadlessOptions, RejectsEmptyCapabilityListMembers) {
    const std::vector<std::vector<std::string>> cases = {
        {"-p", "--disable-tools=", "go"},
        {"-p", "--disable-tools=bash,", "go"},
        {"-p", "--enable-skills=,docs", "go"},
        {"-p", "--enable-skills=a,,b", "go"},
        {"-p", "--enable-mcp=   ", "go"},
    };
    for (const auto& tokens : cases) {
        auto o = parse_headless_cli_options(tokens);
        EXPECT_FALSE(o.error.empty());
        EXPECT_NE(o.error.find("empty name"), std::string::npos) << o.error;
    }
}

// 场景:runner 的三类运行时名称都走同一个 exact selector。
// 期望:available 排序去重;selected / unknown 保持调用顺序并去重;大小写
// 不匹配不做宽松猜测。
TEST(HeadlessNameSelection, ResolvesExactNamesDeterministically) {
    auto result = acecode::headless::select_exact_names(
        {"file_write", "bash", "Bash", "file_write", "missing", "missing"},
        {"grep", "bash", "file_write", "bash"});

    EXPECT_FALSE(result.valid());
    EXPECT_EQ(result.selected,
              (std::vector<std::string>{"file_write", "bash"}));
    EXPECT_EQ(result.unknown,
              (std::vector<std::string>{"Bash", "missing"}));
    EXPECT_EQ(result.available,
              (std::vector<std::string>{"bash", "file_write", "grep"}));
    EXPECT_EQ(acecode::headless::format_name_list(result.available),
              "bash, file_write, grep");
    EXPECT_EQ(acecode::headless::format_name_list({}), "(none)");
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

// ---- 连续对话 flags(-c / --resume / --session-id / --output-format) ----

// 场景:-c / --continue 两种写法,接当前目录最近会话。
// 期望:continue_latest 置位,prompt 不受影响。
TEST(HeadlessOptions, ParsesContinueAliases) {
    for (const char* alias : {"-c", "--continue"}) {
        auto o = parse_headless_cli_options({"-p", alias, "next step"});
        EXPECT_TRUE(o.error.empty()) << alias << ": " << o.error;
        EXPECT_TRUE(o.continue_latest) << alias;
        EXPECT_EQ(o.prompt, "next step") << alias;
    }
}

// 场景:--resume 的空格 / 等号两种形式。
// 期望:resume_session_id 落位,prompt 仍取位置参数。
TEST(HeadlessOptions, ParsesResumeWithId) {
    auto space = parse_headless_cli_options(
        {"-p", "--resume", "20260709-134637-3400", "go on"});
    EXPECT_TRUE(space.error.empty()) << space.error;
    EXPECT_EQ(space.resume_session_id, "20260709-134637-3400");
    EXPECT_EQ(space.prompt, "go on");

    auto eq = parse_headless_cli_options({"-p", "--resume=my-task", "go on"});
    EXPECT_TRUE(eq.error.empty()) << eq.error;
    EXPECT_EQ(eq.resume_session_id, "my-task");
}

// 场景:--resume 不带 id(print 模式有位置参数 prompt,裸 --resume 会把
// prompt 吞成 id,所以这里与 TUI 语义刻意不同 —— id 必填,"最近会话"独立
// 给 -c/--continue)。
// 期望:--resume 是最后一个 token 时报 requires;--resume 后面跟 prompt
// (含空格/标点,不满足 id 字符集)时报 invalid,两种手滑都拦下来。
TEST(HeadlessOptions, ResumeRequiresValidId) {
    auto missing = parse_headless_cli_options({"-p", "--resume"});
    EXPECT_FALSE(missing.error.empty());
    EXPECT_NE(missing.error.find("requires"), std::string::npos) << missing.error;

    auto swallowed = parse_headless_cli_options({"-p", "--resume", "fix the bug"});
    EXPECT_FALSE(swallowed.error.empty());
    EXPECT_NE(swallowed.error.find("invalid"), std::string::npos) << swallowed.error;
}

// 场景:--session-id 自定新会话 id(脚本免解析 stdout 即可 --resume)。
// 期望:合法 id 落位;含路径分隔符 / 点号 / 超长(>64)的 id 全部报错 ——
// id 直接成为 <id>.jsonl 文件名,必须挡住路径穿越。
TEST(HeadlessOptions, ParsesAndValidatesSessionId) {
    auto ok = parse_headless_cli_options({"-p", "--session-id", "ci_run-42", "go"});
    EXPECT_TRUE(ok.error.empty()) << ok.error;
    EXPECT_EQ(ok.session_id, "ci_run-42");

    for (const char* bad : {"../evil", "a/b", "a.b", "with space"}) {
        auto o = parse_headless_cli_options({"-p", "--session-id", bad, "go"});
        EXPECT_FALSE(o.error.empty()) << "value: '" << bad << "'";
    }
    auto too_long = parse_headless_cli_options(
        {"-p", std::string("--session-id=") + std::string(65, 'a'), "go"});
    EXPECT_FALSE(too_long.error.empty());
}

// 场景:互斥组合 —— --continue 与 --resume 同给;--session-id(新建)与
// --resume/--continue(续接)同给。
// 期望:全部硬报错,不做静默取舍(脚本里的矛盾参数几乎必是 bug)。
TEST(HeadlessOptions, RejectsConflictingContinuationFlags) {
    auto both = parse_headless_cli_options({"-p", "-c", "--resume", "id-1", "go"});
    EXPECT_FALSE(both.error.empty());

    auto mix1 = parse_headless_cli_options(
        {"-p", "--session-id", "new-1", "--resume", "id-1", "go"});
    EXPECT_FALSE(mix1.error.empty());

    auto mix2 = parse_headless_cli_options({"-p", "--session-id", "new-1", "-c", "go"});
    EXPECT_FALSE(mix2.error.empty());
}

// 场景:--output-format 的合法值(text/json/stream-json,空格 / 等号形式)与非法值。
// 期望:合法值透传;非法值报错且消息带上原值。
TEST(HeadlessOptions, ParsesAndValidatesOutputFormat) {
    auto js = parse_headless_cli_options({"-p", "--output-format", "json", "go"});
    EXPECT_TRUE(js.error.empty()) << js.error;
    EXPECT_EQ(js.output_format, "json");

    auto txt = parse_headless_cli_options({"-p", "--output-format=text", "go"});
    EXPECT_TRUE(txt.error.empty()) << txt.error;
    EXPECT_EQ(txt.output_format, "text");

    auto stream = parse_headless_cli_options(
        {"-p", "--output-format", "stream-json", "go"});
    EXPECT_TRUE(stream.error.empty()) << stream.error;
    EXPECT_EQ(stream.output_format, "stream-json");

    auto bad = parse_headless_cli_options({"-p", "--output-format", "yaml", "go"});
    EXPECT_FALSE(bad.error.empty());
    EXPECT_NE(bad.error.find("yaml"), std::string::npos);
}

// 场景:--thinking 是加法 flag,位置无关;它只由 stream-json renderer 消费,
// parser 不应改变 text/json 的既有格式选择。
TEST(HeadlessOptions, ParsesThinkingFlagWithoutChangingOutputFormat) {
    auto stream = parse_headless_cli_options(
        {"--thinking", "-p", "--output-format=stream-json", "go"});
    EXPECT_TRUE(stream.error.empty()) << stream.error;
    EXPECT_TRUE(stream.include_thinking);
    EXPECT_EQ(stream.output_format, "stream-json");

    auto legacy = parse_headless_cli_options(
        {"-p", "--thinking", "--output-format", "json", "go"});
    EXPECT_TRUE(legacy.error.empty()) << legacy.error;
    EXPECT_TRUE(legacy.include_thinking);
    EXPECT_EQ(legacy.output_format, "json");
}

// 场景:帮助文本同时说明新 JSONL 模式和旧 json 单对象模式,避免脚本用户把
// 二者混为一谈。
TEST(HeadlessOptions, HelpDocumentsAdditiveStreamJsonMode) {
    const auto help = acecode::headless::print_mode_help();
    EXPECT_NE(help.find("text (default) | json | stream-json"), std::string::npos);
    EXPECT_NE(help.find("json: stdout gets one result object"), std::string::npos);
    EXPECT_NE(help.find("--thinking"), std::string::npos);
    EXPECT_NE(help.find("--disable-tools"), std::string::npos);
    EXPECT_NE(help.find("--enable-skills"), std::string::npos);
    EXPECT_NE(help.find("--enable-mcp"), std::string::npos);
    EXPECT_NE(help.find("default: unlimited"), std::string::npos);
    EXPECT_NE(help.find("Skills"), std::string::npos);
}

// 场景:-p 模式里的 -h / --help。
// 期望:show_help 置位且无解析错误(main.cpp 对 show_help 优先出帮助)。
TEST(HeadlessOptions, ParsesHelpFlag) {
    for (const char* alias : {"-h", "--help"}) {
        auto o = parse_headless_cli_options({"-p", alias});
        EXPECT_TRUE(o.error.empty()) << alias << ": " << o.error;
        EXPECT_TRUE(o.show_help) << alias;
    }
}

// 场景:--session-id 传入形如 <YYYYMMDD-HHMMSS-XXXX>-<digits> 的 id。这种
// 文件名与旧 PID 后缀实验数据无法区分,SessionStorage::list_sessions 会把
// 它当 legacy 数据隐藏 —— 创建出来的会话在所有列表里隐身。
// 期望:解析层直接拒绝;字符集相同但不含 canonical 前缀的 id(foo-123)
// 不受影响。
TEST(HeadlessOptions, RejectsSessionIdShapedLikeLegacyPidFile) {
    auto bad = parse_headless_cli_options(
        {"-p", "--session-id", "20260426-100000-abcd-9999", "go"});
    EXPECT_FALSE(bad.error.empty());
    EXPECT_NE(bad.error.find("legacy"), std::string::npos) << bad.error;

    auto ok = parse_headless_cli_options({"-p", "--session-id", "foo-123", "go"});
    EXPECT_TRUE(ok.error.empty()) << ok.error;
    EXPECT_EQ(ok.session_id, "foo-123");
}

// 场景:is_valid_session_id_token 的边界 —— 恰好 64 字符(合法上限)与
// 65 字符(超限);空串。
// 期望:64 过,65 与空串拒。上限防的是把整段 prompt 误传成 id 的极端场景。
TEST(HeadlessOptions, SessionIdTokenBoundaries) {
    EXPECT_TRUE(acecode::headless::is_valid_session_id_token(std::string(64, 'x')));
    EXPECT_FALSE(acecode::headless::is_valid_session_id_token(std::string(65, 'x')));
    EXPECT_FALSE(acecode::headless::is_valid_session_id_token(""));
}
