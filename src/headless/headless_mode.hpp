#pragma once

// Headless(-p / --print)模式的进程级标记(openspec add-headless-print-mode)。
//
// `acecode -p "prompt"` 整个进程都是无头的:没有 TUI overlay,没有浏览器 WS
// 通道,任何"等用户点确认/答问题"的路径都会永远挂起。所以这个标记做成
// **进程级** 而不是 per-session 字段 —— 好处是 spawn_subagent 派生的子会话
// 自动继承同样的无头行为(同进程),不需要在 SessionOptions 里再穿一层。
// 先例:lsp::init / web_search::runtime 同样是进程级单例。
//
// 两个消费点:
//   1. AgentLoop 权限门(agent_loop.cpp):hook 都没放行、即将走交互 prompt
//      时 —— dangerous(--yolo)下自动放行(含 yolo 外部写首确认),否则直接
//      拒绝并给模型解释性文案,防止 AsyncPrompter 空等 5 分钟超时。
//   2. AskUserQuestion 工具(ask_user_question_tool.cpp):直接返回
//      make_headless_ask_result() 自动应答,指示模型自行决策并继续。
//
// TUI / daemon 路径从不调用 set_active(true),行为零变化。

namespace acecode::headless {

// 仅由 -p 模式入口(run_print_mode)在启动时调用一次。
void set_active(bool on);

// 权限门 / AskUserQuestion 的探针。默认 false。
bool active();

} // namespace acecode::headless
