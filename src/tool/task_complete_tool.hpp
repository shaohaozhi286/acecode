#pragma once

#include "tool_executor.hpp"

namespace acecode {

// `task_complete` 是 agent loop 的显式终止信号工具。
// 模型调用它告知 "用户原请求已全部完成",AgentLoop 看到成功的 task_complete
// tool call 就退出循环。工具本身零副作用 —— 不碰文件系统 / 网络 / 子进程,
// 返回值仅作为 tool_result 进入历史。
//
// 必需参数:summary (string,非空)。用于在 TUI 渲染为一行完成摘要,并让
// 用户一眼看清模型"认为自己做完了什么"。
ToolImpl create_task_complete_tool();

} // namespace acecode
