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
// daemon 与 TUI 共用:daemon 在 worker.cpp 注册(deps 用 shared_ptr 延迟
// 回填,ToolExecutor 先于 SessionRegistry 构造);TUI 通过
// tui::SubagentHost 提供进程内 SessionRegistry 后同样注册。TUI 主会话
// 不在 registry 里,权限模式经 fallback_permissions 继承。

#include "tool_executor.hpp"

#include <functional>
#include <memory>
#include <string>

namespace acecode {

struct AppConfig;
class PermissionManager;
class SessionRegistry;
class SessionClient;

struct SubagentToolDeps {
    SessionRegistry* registry = nullptr;
    SessionClient*   client   = nullptr;
    const AppConfig* config   = nullptr;
    // 父会话不在 registry 中(TUI 主会话)时,子会话权限模式从这里继承。
    const PermissionManager* fallback_permissions = nullptr;
    // spawn 成功后回调(child_id, 原始 prompt)。TUI 用它登记右侧任务列
    // 并订阅子会话事件;daemon 留空(Web 走 tool_end metadata + 事件流)。
    std::function<void(const std::string& child_id,
                       const std::string& prompt)> on_spawn;
};

ToolImpl create_spawn_subagent_tool(std::shared_ptr<SubagentToolDeps> deps);
ToolImpl create_wait_subagent_tool(std::shared_ptr<SubagentToolDeps> deps);

} // namespace acecode
