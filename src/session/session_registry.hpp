#pragma once

// SessionRegistry: daemon 进程内多 session 的注册表(openspec add-web-daemon
// 任务 7.2)。TUI 模式不用这个 — TUI 直接持有一个 AgentLoop。
//
// 每个 session 是一个 SessionEntry,内部持有:
//   - AgentLoop(独立 worker thread)
//   - SessionManager(独立 jsonl + meta,带本进程 pid 后缀)
//   - PermissionManager(独立 session_allowed_,mode/rules 从全局复制)
//   - AsyncPrompter(注入到 AgentLoop)
//
// 共享(整个 daemon 一份):
//   - ProviderAccessor (provider 实例 + 锁,daemon 启动时建立)
//   - ToolExecutor (工具注册表 + MCP)
//   - 全局 cwd
//   - SkillRegistry / MemoryRegistry / 各种 *Config

#include "../agent_loop.hpp"
#include "../config/config.hpp"
#include "../permissions.hpp"
#include "../provider/llm_provider.hpp"
#include "../tool/tool_executor.hpp"
#include "ask_user_question_prompter.hpp"
#include "permission_prompter.hpp"
#include "session_client.hpp"
#include "session_manager.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace acecode {

class SkillRegistry;
class MemoryRegistry;

// SessionEntry: 一个 session 的所有 per-session 状态。所有指针成员都是
// unique_ptr,SessionEntry 析构时按相反顺序销毁(AgentLoop 先 shutdown
// worker thread,再销毁 prompter,再销毁 SessionManager)。
struct SessionEntry {
    std::string id;
    std::string cwd;
    std::string workspace_hash;
    std::string provider;
    std::string model;
    std::unique_ptr<SessionManager>     sm;
    std::unique_ptr<PermissionManager>  perm;
    std::unique_ptr<AgentLoop>           loop;
    AsyncPrompter*                       prompter = nullptr; // owned by loop after move
    // AskUserQuestionPrompter 不被 AgentLoop 持有(AgentLoop 只装 PermissionPrompter),
    // 由 SessionEntry 直接持有 + 通过 ToolContext::ask_question_prompter 注入到
    // 每次工具调用的 ctx。生命周期 = SessionEntry。
    std::unique_ptr<AskUserQuestionPrompter> ask_prompter;
};

// SessionRegistryDeps: 构造 SessionRegistry 时一次性传入的"全局共享物"。
// 通过引用持有,生命周期由调用方保证(典型: daemon worker.cpp)。
struct SessionRegistryDeps {
    AgentLoop::ProviderAccessor      provider_accessor;
    ToolExecutor*                    tools = nullptr;
    std::string                      cwd;
    const AppConfig*                 config = nullptr;
    const SkillRegistry*             skill_registry = nullptr;
    const MemoryRegistry*            memory_registry = nullptr;
    const MemoryConfig*              memory_cfg = nullptr;
    const ProjectInstructionsConfig* project_instructions_cfg = nullptr;
    // 全局 PermissionManager(用于派生 per-session perm 的 mode + rules
    // 起始值)。每个 session 自己的 PermissionManager 是独立实例,session_allowed_
    // 不串。
    const PermissionManager*         template_permissions = nullptr;
};

class SessionRegistry {
public:
    explicit SessionRegistry(SessionRegistryDeps deps);
    ~SessionRegistry();

    SessionRegistry(const SessionRegistry&) = delete;
    SessionRegistry& operator=(const SessionRegistry&) = delete;

    // 创建新 session。返回的 id 会预分配给 SessionManager,所以后续首次落盘
    // 使用同一个 `<session-id>-<pid>.jsonl`,不会出现 registry id 与磁盘 id
    // 分裂。
    std::string create(const SessionOptions& opts);

    // 从磁盘历史恢复 session 到 daemon 内存 registry。若 id 已 active,直接
    // 返回 true;若磁盘没有该 id,返回 false。同一 daemon 不允许同 id 双上下文,
    // Desktop 多上下文由多个 daemon + 不同 pid 隔离。
    bool resume(const std::string& id, const SessionOptions& opts = {});

    // 找一个 entry。返回 nullptr 表示不存在。返回 raw 指针由调用者立刻使用,
    // **不要存储**(随时可能被 destroy)。线程安全:加锁拷贝 raw 指针,但
    // entry 自身 lifetime 不在锁内保证 — 设计上 destroy 是显式的,客户代码
    // 不应在调 destroy 时同时持指针。
    SessionEntry* lookup(const std::string& id);

    // 销毁 session。abort 当前 LLM/tool + join worker + 移出 map。
    void destroy(const std::string& id);

    // 列出当前 daemon 内活跃 session 的元数据(只看内存,不读磁盘历史)。
    std::vector<SessionInfo> list_active() const;

    // 当前活跃 session 数(测试 / 监控用)。
    std::size_t size() const;

private:
    std::unique_ptr<SessionEntry> make_entry_locked(const std::string& id,
                                                     const SessionOptions& opts,
                                                     const std::string& provider,
                                                     const std::string& model);
    void restore_loop_history(SessionEntry& entry,
                              const std::vector<ChatMessage>& messages) const;

    SessionRegistryDeps                                          deps_;
    mutable std::mutex                                            mu_;
    std::unordered_map<std::string, std::unique_ptr<SessionEntry>> entries_;
};

} // namespace acecode
