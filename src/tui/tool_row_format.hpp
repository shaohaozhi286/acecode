#pragma once

// 工具调用行的 Claude Code 风格格式化(redesign-tui-tool-rows):
// 默认 ` ● ToolName`,全局 verbose 时 ` ● ToolName(args)`。
// 纯字符串/状态逻辑,无 FTXUI 依赖,编进 acecode_testable 供单测。

#include <string>
#include <vector>

#include "tui_state.hpp"

namespace acecode { namespace tui {

// 工具行指示灯三态:Pending = 已派发还没有配对结果(执行中或被 abort 丢弃),
// Ok = 配对到成功结果,Failed = 配对到失败结果。
enum class ToolCallDot { Pending, Ok, Failed };

// 从 tool_call 行拆出「工具名 + 参数预览」。
//   content          — legacy 格式 "[Tool: NAME] ARGS_JSON"(agent_loop 派发)。
//   display_override — build_tool_call_preview 的紧凑预览 "label␣␣args"
//                      (label 与参数之间恒为两个空格,label 自身不含双空格)。
// name 取 content 里的真实工具名;args 优先取 display_override 的参数段
// (人类可读),display_override 为空时回退到 content 里的原始 JSON。
// content 不符合 legacy 格式时 name 为空,调用方应整行原样降级渲染。
struct ToolRowParts {
    std::string name;
    std::string args;
};
ToolRowParts parse_tool_row(const std::string& content,
                            const std::string& display_override);

// snake_case → PascalCase:"file_read" → "FileRead","bash" → "Bash"。
// 已经是大写开头的段保持不变(AskUserQuestion 原样);非 ASCII 字节原样
// 透传(中文工具名不动);连续下划线按单个分隔符处理。
std::string pascal_case_tool_name(const std::string& name);

// tool_result 行是否为失败态。与渲染层给结果行标红的判定保持一致:
// "[Error]" 前缀,或 summary metrics 里 exit != 0 / aborted / timeout。
bool tool_result_row_failed(const TuiState::Message& msg);

// FIFO 配对 tool_call ↔ tool_result,给每条消息算指示灯状态(仅 tool_call
// 下标位置有意义,其余为 Pending 占位)。并行批次的派发顺序是先推全部
// tool_call 行、结果行随后按原始顺序追加,所以「最早的未配对调用 ↔ 最早
// 的结果」即正确配对;遇到任何其他角色(assistant/user/system)说明批次
// 已结束,清空未配对队列,防止 abort 残留的调用行错配到下一轮的结果。
std::vector<ToolCallDot> compute_tool_call_dots(
    const std::vector<TuiState::Message>& conversation);

}} // namespace acecode::tui
