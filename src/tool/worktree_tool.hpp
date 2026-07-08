#pragma once

#include "../config/config.hpp"
#include "tool_executor.hpp"

namespace acecode {

// Worktree 隔离工具(复刻 Claude Code EnterWorktreeTool / ExitWorktreeTool)。
//
// `EnterWorktree`:在主仓 .acecode/worktrees/<slug> 创建(或复用)一个
// linked worktree,分支 worktree-<slug> 基于 origin/<默认分支>(本地没有
// 就 fetch,再不行回退当前 HEAD),然后把当前会话的工作目录切进去。
// 会话状态(WorktreeSessionInfo)写进 SessionManager 并持久化到 meta,
// resume 时恢复。仅在用户明确要求 worktree 时使用 —— 工具描述里写死了
// 这个约束,模型不应主动调它。
//
// `ExitWorktree`:退出本会话由 EnterWorktree 建立的 worktree 会话,恢复
// 原工作目录。action=keep 保留目录与分支;action=remove 删除两者,但有
// 未提交文件或基线之后的提交时 fail-closed 拒绝,必须显式
// discard_changes=true 才放行。不碰手工 `git worktree add` 建的或上个
// 会话遗留的 worktree(判据 = SessionManager 里的活动状态)。
//
// 依赖 ToolContext:session_manager(状态持久化)+ switch_session_cwd
// (AgentLoop 注入的 cwd 切换回调);两者任一缺失时工具报错不动状态。
ToolImpl create_enter_worktree_tool(const WorktreeConfig& config);
ToolImpl create_exit_worktree_tool();

} // namespace acecode
