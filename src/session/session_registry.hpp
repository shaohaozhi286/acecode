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
#include "../experts/expert_registry.hpp"
#include "../skills/skill_registry.hpp"
#include "../tool/tool_executor.hpp"
#include "ask_user_question_prompter.hpp"
#include "permission_prompter.hpp"
#include "session_client.hpp"
#include "session_manager.hpp"

#include <atomic>
#include <functional>
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
class ActiveSessionPowerGuard;
class ConnectorAuthRecovery;

std::string default_no_workspace_cache_root();
std::string no_workspace_session_cwd(const std::string& session_id,
                                     const std::string& cache_root = {});
std::vector<std::string> list_no_workspace_session_cwds(const std::string& cache_root = {});

// no-workspace 会话的兜底 meta 查找:先按 id 推导的直连缓存目录读 meta,
// 未命中再枚举缓存根下全部会话目录逐个尝试;只认 meta.no_workspace 的命中。
// 这类会话的 meta 落在 cache/no-workspace/<id>/ 对应的项目目录下,按调用方
// cwd 的常规 workspace 解析永远找不到 —— web resume 路由与 remote-control
// binder 的启动重建共用这一份兜底。
std::optional<SessionMeta> find_no_workspace_session_meta(const std::string& id,
                                                          const std::string& cache_root = {});

// SessionEntry: 一个 session 的所有 per-session 状态。所有指针成员都是
// unique_ptr,SessionEntry 析构时按相反顺序销毁(AgentLoop 先 shutdown
// worker thread,再销毁 prompter,再销毁 SessionManager)。
struct SessionEntry {
    std::string id;
    std::string cwd;
    std::string workspace_hash;
    bool no_workspace = false;
    // spawn_subagent 派生深度(0 = 普通会话)。见 SessionOptions::subagent_depth。
    int subagent_depth = 0;
    // 非空 = spawn_subagent 子会话的父会话 id(随 meta 持久化,resume 恢复)。
    std::string parent_session_id;
    std::string expert_id;
    std::string expert_member_id;
    bool expert_missing = false;
    std::optional<ExpertDefinition> expert;
    bool loop_execution = false;
    std::string loop_id;
    std::string loop_run_id;
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
    const ExpertRegistry*            expert_registry = nullptr;
    const MemoryRegistry*            memory_registry = nullptr;
    const MemoryConfig*              memory_cfg = nullptr;
    const ProjectInstructionsConfig* project_instructions_cfg = nullptr;
    const CustomInstructionsConfig*  custom_instructions_cfg = nullptr;
    HookManager*                     hook_manager = nullptr;
    // 全局 PermissionManager(用于派生 per-session perm 的 mode + rules
    // 起始值)。每个 session 自己的 PermissionManager 是独立实例,session_allowed_
    // 不串。
    PermissionManager*               template_permissions = nullptr;
    // Optional process-wide power guard. When present, every session loop
    // contributes its busy/idle transitions to this guard.
    ActiveSessionPowerGuard*          power_guard = nullptr;
    // Optional connector auth recovery service. When present, every session
    // loop retries once after a successful on_auth_error hook run.
    ConnectorAuthRecovery*           auth_recovery = nullptr;
    // Empty = ~/.acecode/cache/no-workspace. Tests may override to avoid
    // creating cache directories in the real user home.
    std::string                      no_workspace_cache_root;
    // Optional hidden-title runner override for embedders and deterministic
    // registry tests. Production callers leave this empty and use the
    // configured model provider.
    std::function<std::optional<std::string>(const std::string&)>
                                     auto_title_generator;
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
    // Recompute and publish only the context budget for active sessions that
    // still reference model_name. Stable-name edits only: a renamed profile is
    // intentionally not treated as the same reference.
    std::size_t sync_model_context_window(const std::string& model_name,
                                          const ModelProfile& profile);
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

    // 宿主(daemon worker)可注册的兜底命令处理器:内置命令名之外的请求交给
    // 它(如 /rc 的 daemon 托管实现)。未注册时保持 UnsupportedCommand 原语义。
    // handler 在 HTTP 线程上被调,不持 registry 锁;传空清除。
    using ExternalCommandHandler = std::function<BuiltinCommandResult(
        const std::string& session_id, const BuiltinCommandRequest& request)>;
    void set_external_command_handler(ExternalCommandHandler handler);

    // Run a one-turn, tool-free question against the latest provider-facing
    // context snapshot. This never mutates AgentLoop/SessionManager state.
    SideQuestionResult ask_side_question(const std::string& id,
                                         const std::string& question);

    // Resolve persisted metadata to displayable model state without activating
    // the session. Used by web endpoints for inactive disk sessions.
    std::optional<SessionModelState> model_state_from_meta(const SessionMeta& meta) const;

    // 当前活跃 session 数(测试 / 监控用)。
    std::size_t size() const;

    // ---- git 感知辅助(openspec add-webui-git-session-pill)----

    // workspace cwd 下是否有会话正在跑回合。checkout 安全门:agent 写文件
    // 写一半被切分支是数据灾难,保守到整个 workspace 粒度。
    bool any_busy_in_cwd(const std::string& cwd) const;

    // checkout 成功后标记该 workspace 全部会话的 gitStatus 快照过期
    // (AgentLoop::invalidate_git_snapshot,线程安全)。
    void invalidate_git_snapshots_in_cwd(const std::string& cwd);

    // Web 首条消息的 worktree 前置步骤:为"尚无消息"的会话创建(或复用)
    // `ses-<id前8位>` worktree(基线 = base_branch,空 = 默认 origin/<默认分支>
    // 策略)并切会话 cwd。会话存储位置不动(与 EnterWorktree 工具同语义)。
    struct WebWorktreeResult {
        bool ok = false;
        int http_status = 500;     // 失败时给路由层的语义状态码
        std::string error;
        std::string worktree_path;
        std::string worktree_branch;
    };
    WebWorktreeResult enter_worktree_for_web(const std::string& id,
                                             const std::string& base_branch);

private:
    std::shared_ptr<SessionEntry> make_entry_locked(const std::string& id,
                                                     const SessionOptions& opts,
                                                     const SessionMeta* resumed_meta);
    void restore_loop_history(SessionEntry& entry,
                              const std::vector<ChatMessage>& messages) const;
    void start_auto_title_attempt(const std::string& id, std::string text);
    void handle_auto_title_turn_finished(const std::string& id,
                                         const std::string& status);

    SessionRegistryDeps                                          deps_;
    mutable std::mutex                                            mu_;
    std::unordered_map<std::string, std::shared_ptr<SessionEntry>> entries_;
    mutable std::mutex                                            external_handler_mu_;
    ExternalCommandHandler                                        external_command_handler_;
    mutable std::mutex                                            title_threads_mu_;
    std::vector<std::thread>                                      title_threads_;
    std::atomic<bool>                                              shutting_down_{false};
};

} // namespace acecode
