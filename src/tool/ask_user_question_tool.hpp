#pragma once

#include "tool_executor.hpp"

#include <ftxui/component/screen_interactive.hpp>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace acecode {

struct TuiState;

// 单个选项:label 是模型给出的显示文本,description 是可选的解释文本。
// preview 字段上游 claudecodehaha 支持,但 ACECode 的 TUI 当前不渲染 preview,
// 仅作为 schema 兼容层允许传入(parse 时忽略内容)。
struct AskOption {
    std::string label;
    std::string description;
};

struct AskQuestion {
    std::string question;     // 问题文本 (必须以 `?` 结尾 —— 但 schema 不强制,交给模型)
    std::string header;       // 12 字符以内的 chip 标签
    std::vector<AskOption> options;  // 2–4 个显式选项;"Other..." 由 TUI 自动追加
    bool multi_select = false;
};

// 解析 + 校验 `AskUserQuestion` 工具的 JSON 参数。成功时返回解析出来的
// question 列表,失败时返回 std::nullopt 并把错误消息写入 `err`
// (以 "questions" / "options" / "unique" / "labels" / "header" 等关键词
// 为索引供上层匹配)。纯函数 —— 不碰 TuiState,供单测直接调用。
std::optional<std::vector<AskQuestion>> validate_ask_user_question_args(
    const std::string& arguments_json, std::string& err);

// 拼接最终的 ToolResult 输出字符串。question_order 保留模型给问题的原始顺序,
// answers 的 value 对于 multi-select 是调用方已经用 ", " 拼好的单一字符串。
// 返回形如 `User has answered your questions: "Q1"="A1", "Q2"="A2"` 的单行。
std::string format_ask_answers(
    const std::vector<std::string>& question_order,
    const std::map<std::string, std::string>& answers);

// 拒绝路径(Esc / agent abort)的固定 ToolResult:success=false,
// output="[Error] User declined to answer questions."
ToolResult make_rejected_ask_result();

// 工厂函数:新建 AskUserQuestion 工具。TuiState 引用用于发起阻塞 overlay,
// screen 引用用于 PostEvent 唤醒渲染线程。工具内部通过 ToolContext::abort_flag
// 感知 agent 中止。
ToolImpl create_ask_user_question_tool(TuiState& state,
                                        ftxui::ScreenInteractive& screen);

// daemon 路径用的工厂。execute() 不碰 TuiState/ScreenInteractive,完全靠
// `ToolContext::ask_user_questions` 异步通道(典型实现: 走 WS question_request
// → 浏览器 modal → question_answer 回流)。ctx.ask_user_questions 为空时
// 直接返回 make_rejected_ask_result()(daemon 没装 prompter = AskUserQuestion
// 在该会话不可用)。
ToolImpl create_ask_user_question_tool_async();

} // namespace acecode
