#pragma once

// SessionRegistry: daemon 进程内多 session 的注册表(openspec add-web-daemon
// 任务 7.2)。TUI 模式不用这个 — TUI 直接持有一个 AgentLoop。
//
// 每个 session 是一个 SessionEntry,内部持有:
//   - AgentLoop(独立 worker thread)
//   - SessionManager(独立 in-memory state; persisted as canonical jsonl + meta)
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
#include "../config/saved_models.hpp"
#include "../skills/skill_registry.hpp"
#include "../tool/tool_executor.hpp"
#include "ask_user_question_prompter.hpp"
#include "permission_prompter.hpp"
#include "session_client.hpp"
#include "session_manager.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace acecode {

class HookManager;
class MemoryRegistry;

// SessionEntry: 一个 session 的所有 per-session 状态。所有指针成员都是
// unique_ptr,SessionEntry 析构时按相反顺序销毁(AgentLoop 先 shutdown
// worker thread,再销毁 prompter,再销毁 SessionManager)。
struct SessionEntry {
    std::string id;
    std::string cwd;
    std::string workspace_hash;
    bool no_workspace = false;
    std::string provider;
    std::string model;
    SessionModelState model_state;
    struct ProviderSlot {
        mutable std::mutex mu;
        std::shared_ptr<LlmProvider> provider;
    };
    std::shared_ptr<ProviderSlot> provider_slot;
    std::unique_ptr<SkillRegistry>       skill_registry;
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
    const CustomInstructionsConfig*  custom_instructions_cfg = nullptr;
    HookManager*                     hook_manager = nullptr;
    // 全局 PermissionManager(用于派生 per-session perm 的 mode + rules
    // 起始值)。每个 session 自己的 PermissionManager 是独立实例,session_allowed_
    // 不串。
    PermissionManager*               template_permissions = nullptr;
};

class SessionRegistry {
public:
    explicit SessionRegistry(SessionRegistryDeps deps);
    ~SessionRegistry();

    SessionRegistry(const SessionRegistry&) = delete;
    SessionRegistry& operator=(const SessionRegistry&) = delete;

    // 创建新 session。返回的 id 会预分配给 SessionManager,所以后续首次落盘
    // 使用同一个 canonical `<session-id>.jsonl`,不会出现 registry id 与磁盘 id
    // 分裂。
    std::string create(const SessionOptions& opts);

    // 从磁盘历史恢复 session 到 daemon 内存 registry。若 id 已 active,直接
    // 返回 true;若磁盘没有该 id,返回 false。同一 daemon 不允许同 id 双上下文。
    bool resume(const std::string& id, const SessionOptions& opts = {});

    // 找一个 entry 并延长其生命周期。HTTP/WS handler 等跨锁使用 entry 的
    // 调用方应使用 acquire(),避免 destroy() 并发释放对象。
    std::shared_ptr<SessionEntry> acquire(const std::string& id);

    // 兼容旧测试/少量即时读取路径。返回 raw 指针由调用者立刻使用,不要存储;
    // 需要跨锁使用时必须改用 acquire()。
    SessionEntry* lookup(const std::string& id);

    // 销毁 session。abort 当前 LLM/tool + join worker + 移出 map。
    void destroy(const std::string& id);

    // 列出当前 daemon 内活跃 session 的元数据(只看内存,不读磁盘历史)。
    std::vector<SessionInfo> list_active() const;

    // Read or switch the effective model for one active session. Switching
    // affects future turns for that session only; an in-flight turn keeps the
    // provider shared_ptr snapshot it already captured.
    std::optional<SessionModelState> current_model_state(const std::string& id) const;
    bool model_profile_used_by_busy_session(const std::string& model_name) const;
    bool switch_model(const std::string& id,
                      const ModelProfile& profile,
                      SessionModelState* out = nullptr,
                      std::string* error = nullptr);

    // Current active session permission mode. These are intentionally
    // session-scoped so Web UI changes do not affect unrelated sessions.
    std::optional<PermissionMode> permission_mode(const std::string& id) const;
    bool set_permission_mode(const std::string& id, PermissionMode mode);

    // Fire-and-forget hidden title generation for the first visible user input.
    // It never writes to transcript or blocks send_input.
    void maybe_start_auto_title(const std::string& id, const UserInput& input);

    // Daemon-wide default for sessions created after this call. Existing
    // sessions remain session-scoped and are not mutated here.
    PermissionMode default_permission_mode() const;
    void set_default_permission_mode(PermissionMode mode);

    BuiltinCommandResult execute_builtin_command(
        const std::string& id,
        const BuiltinCommandRequest& request);

    // Resolve persisted metadata to displayable model state without activating
    // the session. Used by web endpoints for inactive disk sessions.
    std::optional<SessionModelState> model_state_from_meta(const SessionMeta& meta) const;

    // 当前活跃 session 数(测试 / 监控用)。
    std::size_t size() const;

private:
    std::shared_ptr<SessionEntry> make_entry_locked(const std::string& id,
                                                     const SessionOptions& opts,
                                                     const SessionMeta* resumed_meta);
    void restore_loop_history(SessionEntry& entry,
                              const std::vector<ChatMessage>& messages) const;

    SessionRegistryDeps                                          deps_;
    mutable std::mutex                                            mu_;
    std::unordered_map<std::string, std::shared_ptr<SessionEntry>> entries_;
    mutable std::mutex                                            title_threads_mu_;
    std::vector<std::thread>                                      title_threads_;
};

} // namespace acecode
