#pragma once

// Resume 时把磁盘上的 OpenAI 规范 role 序列展开为 TUI 渲染期望的伪角色行序列。
//
// 背景:运行时 TUI 渲染按伪角色 (`tool_call` / `tool_result`) 派发,这些标签由
// agent_loop 的 callbacks 在 push 进 state.conversation 时即时打;但磁盘 JSONL
// 只存 OpenAI 规范 role (user / assistant / system / tool)。Resume 路径必须
// 自己做这次"规范 → 伪角色"的翻译,否则 assistant+tool_calls 显示空白、
// role=tool 整条消失。
//
// 本函数纯函数:无 IO、无全局状态、无 FTXUI 依赖,可链入 acecode_testable
// 单测。Daemon / Web UI 未来要做同样的展开时,可直接复用这个函数的逻辑。

#include "../tui_state.hpp"
#include "../provider/llm_provider.hpp"

#include <vector>

namespace acecode {

class ToolExecutor;  // 前向声明:函数只用 const 引用调 build_tool_call_preview

// 把规范 role 列表展开为 TUI Message 列表。展开规则:
//   - user / system   → 原样推入(role/content/false)
//   - assistant       → 若 content 非空先推一行 {assistant};再对每个 tool_calls[i]
//                        推一行 {tool_call, "[Tool: NAME] ARGS", true},display_override
//                        由 ToolExecutor::build_tool_call_preview 现算
//   - tool            → {tool_result, content, true},尝试从 metadata.tool_summary /
//                        metadata.tool_hunks 还原 summary / hunks(无则降级 fold)
//   - 其他            → 原样推入(forward-compat 兜底,role/content/false)
//
// 注意:shell-mode 的 `!user + tool_result` 配对识别由调用方先做;走到这里的
// 都已经不是配对的一部分。tool_result 这种伪角色如果落到这里,会走 unknown
// 分支原样 push —— 这是正确的降级。
std::vector<TuiState::Message> replay_session_messages(
    const std::vector<ChatMessage>& messages,
    const ToolExecutor& tools);

} // namespace acecode
