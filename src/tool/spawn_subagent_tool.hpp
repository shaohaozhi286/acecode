#pragma once

// spawn_subagent / wait_subagent — daemon 模式的通用子代理工具。
//
// 设计参照 Claude Code 的 Task 工具语义,落在 ACECode 的多 session 基建上:
//   - 子代理 = SessionRegistry 里的一个普通 session(独立 SessionManager /
//     AgentLoop / PermissionManager / ProviderSlot),上下文与父会话完全隔离;
//     它出现在侧栏里,用户可随时点进去围观或接管。
//   - spawn_subagent(prompt, wait=true):创建子会话并注入首条消息。
//     wait=true 阻塞至子会话本轮结束,把最终 assistant 答复带回父上下文
//     (父上下文只多这一条摘要,不吃子会话的中间过程);
//     wait=false 点火即返(fire-and-forget),用于流水线接力 —— 阶段 A 结束
//     时点火阶段 B 的会话,父会话不吃任何 B 的输出。
//   - prompt 支持 `/skill-name args` 形式:与 Web 输入框一致地走
//     try_expand_skill_command 展开为轻量 activation 提示。
//   - wait_subagent(session_id):等待某个已点火的子会话空闲并取回其最新
//     答复。配合 spawn(wait=false) 实现「并行 fan-out 再逐个 join」。
//
// 深度限制:子代理不能再派生子代理(与 Claude Code 一致)—— 通过
// SessionEntry::subagent_depth 判定,防止失控的递归派生。
//
// 仅 daemon 路径注册(TUI 单 session 无 SessionRegistry)。deps 用
// shared_ptr 延迟回填:worker.cpp 里 ToolExecutor 先于 SessionRegistry
// 构造,注册时字段为空,registry/client 就绪后填入(发生在 server 启动前,
// 无并发窗口)。

#include "tool_executor.hpp"

#include <memory>

namespace acecode {

struct AppConfig;
class SessionRegistry;
class SessionClient;

struct SubagentToolDeps {
    SessionRegistry* registry = nullptr;
    SessionClient*   client   = nullptr;
    const AppConfig* config   = nullptr;
};

ToolImpl create_spawn_subagent_tool(std::shared_ptr<SubagentToolDeps> deps);
ToolImpl create_wait_subagent_tool(std::shared_ptr<SubagentToolDeps> deps);

} // namespace acecode
