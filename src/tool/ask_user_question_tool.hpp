#pragma once

#include "tool_executor.hpp"

#include <ftxui/component/screen_interactive.hpp>
#include <nlohmann/json.hpp>

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

// Build UI-only metadata for answered AskUserQuestion results. The tool output
// remains the provider-visible text contract; UI surfaces use this structured
// payload to render compact confirmation cards.
nlohmann::json build_ask_user_question_result_metadata(
    const std::vector<std::string>& question_order,
    const std::map<std::string, std::string>& answers);

// Build a compact UI-only Q/A transcript from ask_user_question_result
// metadata. Returns empty for missing or malformed metadata.
std::string format_ask_user_question_result_display(
    const nlohmann::json& metadata);

// 拒绝路径(Esc / agent abort)的固定 ToolResult:success=false,
// output="[Error] User declined to answer questions."
ToolResult make_rejected_ask_result();

// Goal 无人值守模式的自动应答 ToolResult:success=true,文案指示模型
// 自行决策并继续推进 goal(不弹 UI、不等待用户)。TUI 与 daemon 两个
// AskUserQuestion 实现共用。
ToolResult make_goal_unattended_ask_result();

// Headless(-p / --print)模式的自动应答 ToolResult:success=true,文案指示
// 模型在 print 模式下自行决策并继续(openspec add-headless-print-mode)。
ToolResult make_headless_ask_result();

// question_policy=deny 的自动应答 ToolResult(add-ask-question-policy)。
// success=true(沿用 goal 无人值守的实证教训:false 会让模型当失败反复
// 重问),文案指示模型选推荐项或最合理假设并继续;metadata 携带
// ask_user_question_auto={mode:"deny", origin} 供转录行标注。origin 传
// ResolvedQuestionPolicy::origin("explicit" / "yolo-implicit")。
ToolResult make_policy_denied_ask_result(const char* origin);

// question_policy=timeout 到期的自动采纳 ToolResult:每个 question 取第一
// 个选项(工具 description 约定推荐项排第一)作为答案,output 前缀注明
// 用户 N 秒未回答、答案是自动采纳而非用户真实意志;metadata 同时携带
// ask_user_question_result(正常答案结构)与 ask_user_question_auto=
// {mode:"timeout", seconds}。TUI 与 daemon 两路共用。
ToolResult make_timeout_adopted_ask_result(
    const std::vector<AskQuestion>& questions,
    const std::vector<std::string>& question_order,
    int timeout_seconds);

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
