// tool_row_format 单元测试(redesign-tui-tool-rows)。
// 覆盖 Claude Code 风格工具行 ` ● ToolName(args)` 的三块纯逻辑:
//   1. pascal_case_tool_name —— snake_case → PascalCase 工具名转换;
//   2. parse_tool_row —— 从 legacy content / display_override 拆名字和参数;
//   3. compute_tool_call_dots —— tool_call ↔ tool_result 的 FIFO 配对指示灯。

#include "tui/tool_row_format.hpp"

#include <gtest/gtest.h>

namespace acecode { namespace tui {

namespace {

TuiState::Message make_msg(const std::string& role, const std::string& content) {
    TuiState::Message m;
    m.role = role;
    m.content = content;
    m.is_tool = (role == "tool_call" || role == "tool_result");
    return m;
}

} // namespace

// ---- pascal_case_tool_name ----

// 场景:内置工具名都是 snake_case(bash / file_read / web_search)。
// 期望:下划线去掉、每段首字母大写,与 Claude Code 的工具名风格一致。
TEST(ToolRowFormatTest, PascalCaseConvertsSnakeCase) {
    EXPECT_EQ(pascal_case_tool_name("bash"), "Bash");
    EXPECT_EQ(pascal_case_tool_name("file_read"), "FileRead");
    EXPECT_EQ(pascal_case_tool_name("web_search"), "WebSearch");
    EXPECT_EQ(pascal_case_tool_name("task_complete"), "TaskComplete");
}

// 场景:本来就是 PascalCase 的工具名(AskUserQuestion / EnterWorktree)。
// 期望:原样保留,不重复转换、不破坏已有大写。
TEST(ToolRowFormatTest, PascalCaseKeepsExistingPascal) {
    EXPECT_EQ(pascal_case_tool_name("AskUserQuestion"), "AskUserQuestion");
    EXPECT_EQ(pascal_case_tool_name("EnterWorktree"), "EnterWorktree");
}

// 场景:MCP 工具名带双下划线(mcp__server__tool);中文标签(子代理预览)。
// 期望:连续下划线按单个分隔符折叠;非 ASCII 字节原样透传不被破坏。
TEST(ToolRowFormatTest, PascalCaseHandlesMcpAndUtf8) {
    EXPECT_EQ(pascal_case_tool_name("mcp__github__search"), "McpGithubSearch");
    EXPECT_EQ(pascal_case_tool_name("启动子代理"), "启动子代理");
    EXPECT_EQ(pascal_case_tool_name(""), "");
}

// ---- parse_tool_row ----

// 场景:工具刚派发,display_override 还没算好(为空),行内只有 legacy
// content "[Tool: bash] {json}"。
// 期望:名字取真实工具名,参数回退到原始 JSON —— 渲染层至少能画出
// `● Bash({"command":"ls"})` 而不是整行 JSON。
TEST(ToolRowFormatTest, ParseLegacyContentOnly) {
    const auto parts = parse_tool_row(
        "[Tool: bash] {\"command\":\"ls\"}", "");
    EXPECT_EQ(parts.name, "bash");
    EXPECT_EQ(parts.args, "{\"command\":\"ls\"}");
}

// 场景:工具完成后 display_override 已填("bash␣␣npm install",label 与
// 参数之间恒为两个空格)。
// 期望:名字仍取 content 里的真实工具名;参数取 display_override 的人类
// 可读段,不再是 JSON。
TEST(ToolRowFormatTest, ParsePrefersDisplayOverrideArgs) {
    const auto parts = parse_tool_row(
        "[Tool: bash] {\"command\":\"npm install\"}",
        "bash  npm install");
    EXPECT_EQ(parts.name, "bash");
    EXPECT_EQ(parts.args, "npm install");
}

// 场景:参数本身含连续空格(bash 命令里写了两个空格)。
// 期望:只在第一处双空格切分,参数内部的双空格原样保留。
TEST(ToolRowFormatTest, ParseSplitsAtFirstDoubleSpaceOnly) {
    const auto parts = parse_tool_row(
        "[Tool: bash] {}",
        "bash  echo \"a  b\"");
    EXPECT_EQ(parts.args, "echo \"a  b\"");
}

// 场景:display_override 只有 label 没有参数段(wait_subagent → "等待子代理")。
// 期望:参数为空,渲染层只画名字不画空括号。
TEST(ToolRowFormatTest, ParseLabelOnlyOverrideYieldsEmptyArgs) {
    const auto parts = parse_tool_row(
        "[Tool: wait_subagent] {}", "等待子代理");
    EXPECT_EQ(parts.name, "wait_subagent");
    EXPECT_TRUE(parts.args.empty());
}

// 场景:无参数工具的 legacy content 参数是空对象 "{}"。
// 期望:视为无参数(空串),避免渲染出 `TaskComplete({})`。
TEST(ToolRowFormatTest, ParseTreatsEmptyJsonObjectAsNoArgs) {
    const auto parts = parse_tool_row("[Tool: task_complete] {}", "");
    EXPECT_EQ(parts.name, "task_complete");
    EXPECT_TRUE(parts.args.empty());
}

// 场景:content 不是 legacy 格式(理论上不该发生,防御路径)。
// 期望:name 为空,调用方据此整行原样降级渲染,不丢信息。
TEST(ToolRowFormatTest, ParseMalformedContentYieldsEmptyName) {
    const auto parts = parse_tool_row("something unexpected", "");
    EXPECT_TRUE(parts.name.empty());
}

// 场景:content 解析失败但 display_override 存在(resume 老数据等边角)。
// 期望:用 override 的 label 兜底当名字,行不至于整行降级。
TEST(ToolRowFormatTest, ParseFallsBackToOverrideLabelAsName) {
    const auto parts = parse_tool_row("garbage", "bash  ls -la");
    EXPECT_EQ(parts.name, "bash");
    EXPECT_EQ(parts.args, "ls -la");
}

// ---- tool_result_row_failed ----

// 场景:工具抛异常/被拒,输出以 "[Error]" 开头(agent_loop 的失败约定)。
// 期望:判失败 —— 与渲染层给结果行标红的分支一致。
TEST(ToolRowFormatTest, ResultFailedByErrorPrefix) {
    EXPECT_TRUE(tool_result_row_failed(
        make_msg("tool_result", "[Error] boom")));
    EXPECT_FALSE(tool_result_row_failed(
        make_msg("tool_result", "ok output")));
}

// 场景:bash 正常返回但退出码非 0 / 被 abort / 超时(summary metrics 记录)。
// 期望:三种 metrics 任一命中都判失败;exit=0 且无异常标记判成功。
TEST(ToolRowFormatTest, ResultFailedBySummaryMetrics) {
    auto ok = make_msg("tool_result", "output");
    ok.summary = ToolSummary{"Ran", "ls", {{"exit", "0"}}, "$"};
    EXPECT_FALSE(tool_result_row_failed(ok));

    auto bad_exit = make_msg("tool_result", "output");
    bad_exit.summary = ToolSummary{"Ran", "ls", {{"exit", "1"}}, "$"};
    EXPECT_TRUE(tool_result_row_failed(bad_exit));

    auto aborted = make_msg("tool_result", "output");
    aborted.summary = ToolSummary{"Ran", "ls", {{"aborted", "true"}}, "$"};
    EXPECT_TRUE(tool_result_row_failed(aborted));

    auto timeout = make_msg("tool_result", "output");
    timeout.summary = ToolSummary{"Ran", "ls", {{"timeout", "true"}}, "$"};
    EXPECT_TRUE(tool_result_row_failed(timeout));
}

// ---- compute_tool_call_dots ----

// 场景:最常见的串行执行 —— call → result 成对相邻。
// 期望:成功结果给绿点(Ok),"[Error]" 结果给红点(Failed)。
TEST(ToolRowFormatTest, DotsSequentialPairing) {
    std::vector<TuiState::Message> conv;
    conv.push_back(make_msg("tool_call", "[Tool: bash] {}"));
    conv.push_back(make_msg("tool_result", "fine"));
    conv.push_back(make_msg("tool_call", "[Tool: bash] {}"));
    conv.push_back(make_msg("tool_result", "[Error] no"));

    const auto dots = compute_tool_call_dots(conv);
    ASSERT_EQ(dots.size(), 4u);
    EXPECT_EQ(dots[0], ToolCallDot::Ok);
    EXPECT_EQ(dots[2], ToolCallDot::Failed);
}

// 场景:并行只读批次 —— agent_loop 先推全部 tool_call 行,结果行随后按
// 原始顺序追加(tc1,tc2,tc3,tr1,tr2,tr3),中间不插其他角色。
// 期望:FIFO 配对,第 k 个结果配第 k 个调用 —— tr2 失败只让 tc2 变红。
TEST(ToolRowFormatTest, DotsParallelBatchFifoPairing) {
    std::vector<TuiState::Message> conv;
    conv.push_back(make_msg("tool_call", "[Tool: file_read] {}"));  // 0
    conv.push_back(make_msg("tool_call", "[Tool: grep] {}"));       // 1
    conv.push_back(make_msg("tool_call", "[Tool: glob] {}"));       // 2
    conv.push_back(make_msg("tool_result", "ok1"));                 // 3
    conv.push_back(make_msg("tool_result", "[Error] ok2"));         // 4
    conv.push_back(make_msg("tool_result", "ok3"));                 // 5

    const auto dots = compute_tool_call_dots(conv);
    EXPECT_EQ(dots[0], ToolCallDot::Ok);
    EXPECT_EQ(dots[1], ToolCallDot::Failed);
    EXPECT_EQ(dots[2], ToolCallDot::Ok);
}

// 场景:回合被 abort,tool_call 永远等不到结果;下一轮用户消息之后又有
// 新的正常 call/result。
// 期望:孤儿调用保持 Pending(灰点);批次边界(user 行)清空未配对队列,
// 新一轮的结果绝不会错配到上一轮的孤儿调用上。
// 回归:没有边界清空时,下一轮的 tr 会 FIFO 配到上一轮孤儿 tc,新调用
// 反而永远 Pending —— 灰绿错位。
TEST(ToolRowFormatTest, DotsAbandonedCallStaysPendingAcrossTurns) {
    std::vector<TuiState::Message> conv;
    conv.push_back(make_msg("tool_call", "[Tool: bash] {}"));   // 0: abort 孤儿
    conv.push_back(make_msg("user", "继续"));                   // 1: 批次边界
    conv.push_back(make_msg("tool_call", "[Tool: bash] {}"));   // 2
    conv.push_back(make_msg("tool_result", "done"));            // 3

    const auto dots = compute_tool_call_dots(conv);
    EXPECT_EQ(dots[0], ToolCallDot::Pending);
    EXPECT_EQ(dots[2], ToolCallDot::Ok);
}

// 场景:assistant 文本前奏夹在两个工具批次之间(常见:模型先说一句再调工具)。
// 期望:assistant 行同样是批次边界;之后的批次配对不受影响。
TEST(ToolRowFormatTest, DotsAssistantTextIsBatchBoundary) {
    std::vector<TuiState::Message> conv;
    conv.push_back(make_msg("tool_call", "[Tool: bash] {}"));   // 0
    conv.push_back(make_msg("tool_result", "ok"));              // 1
    conv.push_back(make_msg("assistant", "接着我要改文件"));    // 2
    conv.push_back(make_msg("tool_call", "[Tool: file_edit] {}")); // 3
    conv.push_back(make_msg("tool_result", "[Error] denied"));  // 4

    const auto dots = compute_tool_call_dots(conv);
    EXPECT_EQ(dots[0], ToolCallDot::Ok);
    EXPECT_EQ(dots[3], ToolCallDot::Failed);
}

}} // namespace acecode::tui
