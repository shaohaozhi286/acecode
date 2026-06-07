#pragma once

#include "provider/llm_provider.hpp"
#include "tool/tool_executor.hpp"
#include "permissions.hpp"
#include "utils/path_validator.hpp"
#include "utils/token_tracker.hpp"
#include "session/session_manager.hpp"
#include "session/event_dispatcher.hpp"
#include "session/permission_prompter.hpp"
#include "session/ask_user_question_prompter.hpp"
#include "config/config.hpp"

#include <vector>
#include <string>
#include <functional>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <queue>
#include <map>
#include <utility>

namespace acecode {

class SkillRegistry;
class MemoryRegistry;
struct MemoryConfig;
struct ProjectInstructionsConfig;
struct CompactResult;

// Callbacks for the TUI to observe agent loop events
struct AgentCallbacks {
    // Called when a new message is added to the conversation
    std::function<void(const std::string& role, const std::string& content, bool is_tool)> on_message;

    // Called after each tool execution with the structured ToolResult so the
    // TUI can render a summary row. Fires in addition to on_message (not in
    // place of it) so consumers that only care about the text stream continue
    // to work unchanged. Receives the tool_call message too so the TUI can
    // correlate summaries with their call rows.
    std::function<void(const ChatMessage& call_msg,
                       const std::string& tool_name,
                       const ToolResult& result)> on_tool_result;

    // Called when the agent starts/stops processing
    std::function<void(bool busy)> on_busy_changed;

    // Called to request user confirmation for a tool call.
    // Returns: Allow, Deny, or AlwaysAllow
    std::function<PermissionResult(const std::string& tool_name, const std::string& arguments)> on_tool_confirm;

    // Called for each streaming delta token (real-time TUI update)
    std::function<void(const std::string& token)> on_delta;

    // Called when token usage data is received from the provider
    std::function<void(const TokenUsage& usage)> on_usage;

    // Called when the current thread goal status changes. Empty string means
    // no goal is active for the current session.
    std::function<void(const std::string& status)> on_goal_status;

    // Called when TodoWrite publishes or reads the current visible checklist.
    // The payload shape matches the todo_updated session event.
    std::function<void(const nlohmann::json& payload)> on_todo_updated;

    // Called after the shared AgentLoop compact path has replaced model
    // history. TUI uses this as a display observer; daemon/Web consumers use
    // the TranscriptReplace event stream instead.
    std::function<void(const std::vector<ChatMessage>& messages,
                       const CompactResult& result)> on_transcript_replace;

    // Called before a provider timeout retry replays the current model request.
    // Consumers should clear provisional live assistant output from the failed
    // stream attempt; persisted history is unchanged.
    std::function<void()> on_stream_retry_reset;

    // Called just before a tool begins executing. `command_preview` is a short
    // human-readable summary (e.g. the first 60 chars of a bash command).
    std::function<void(const std::string& tool_name,
                       const std::string& command_preview)> on_tool_progress_start;

    // Called from the tool's streaming thread with each cleaned chunk.
    // `tail_snapshot` is the last-5-lines sliding window; `current_partial` is
    // the in-progress line (not yet terminated by \n).
    std::function<void(const std::vector<std::string>& tail_snapshot,
                       const std::string& current_partial,
                       size_t total_bytes,
                       int total_lines)> on_tool_progress_update;

    // Called after the tool returns (or throws). Guaranteed via RAII to fire
    // once for every on_tool_progress_start.
    std::function<void()> on_tool_progress_end;
};

class AgentLoop {
public:
    // provider_accessor: 每轮 turn 开始时调用,返回当前有效的 provider 的
    // shared_ptr 快照。调用方负责在该函数内部加锁保护 main.cpp 的 provider
    // 替换(见 design D4 / 任务 4.6)。这样 worker 即使跨 turn 持有 snapshot
    // 也不会悬空,下一轮再拿最新的。
    using ProviderAccessor = std::function<std::shared_ptr<LlmProvider>()>;

    AgentLoop(ProviderAccessor provider_accessor, ToolExecutor& tools,
              AgentCallbacks callbacks, const std::string& cwd,
              PermissionManager& permissions);
    ~AgentLoop();

    void set_callbacks(AgentCallbacks cb);

    // Submit a user message. Non-blocking: enqueues the message and returns immediately.
    // The internal worker thread will process it.
    void submit(const std::string& user_message);

    // Submit with separate "LLM prompt" vs "UI display" texts. `prompt` is what
    // the model sees in `messages_` (and persisted JSONL);`display_text` is
    // recorded in `user_msg.metadata.display_text` so UI can show the original
    // user input even though the model sees an expanded form (e.g. daemon-side
    // skill command expansion). Empty `display_text` falls back to `prompt`.
    void submit(const std::string& prompt, const std::string& display_text);

    // Submit structured user input containing text plus optional attachment or
    // context parts. Existing text-only submit overloads delegate here.
    void submit(const UserInput& input);

    // Submit a user-initiated shell command triggered by `!` mode. Non-blocking:
    // enqueues on the same worker so it serialises with LLM turns. The worker
    // invokes BashTool directly (no LLM round-trip), emits tool_call + tool_result
    // UI messages via callbacks, and appends a `<bash-input>/<bash-stdout>/...`
    // user-role entry to messages_ for the next LLM turn.
    void submit_shell(std::string command);

    // Queue a manual `/compact` control task on the same worker as chat/tool
    // turns so transcript mutation cannot race an active model run.
    void submit_compact();

    // Emit a visible system message without adding it to LLM history. Used by
    // daemon-owned builtin commands for TUI-like progress and fallback output.
    void emit_system_message(const std::string& content);
    void emit_transcript_system_message(const std::string& content,
                                        nlohmann::json metadata = nlohmann::json::object());

    // Append a single user-role entry to messages_ representing an already-run
    // shell command and its captured output. Used both by the shell worker
    // branch and by --resume to rehydrate LLM context from persisted session
    // messages (`!cmd` user + tool_result pair).
    void inject_shell_turn(const std::string& cmd,
                           const std::string& stdout_text,
                           const std::string& stderr_text,
                           int exit_code);

    // Abort the current inference. Safe to call from any thread.
    void abort();
    void clear_stale_abort_request();

    // Signal the worker thread to exit and wait for it to finish.
    void shutdown();

    // Returns true if abort has been requested. Useful for confirm callbacks.
    bool is_aborting() const { return abort_requested_.load(); }

    // Returns true while the worker is processing a submitted turn.
    bool is_busy() const { return busy_.load(); }

    // Legacy cancel alias
    void cancel() { abort(); }

    // Clear all messages (for /clear command)
    void clear_messages() { messages_.clear(); }

    // Push a message (for session restore)
    void push_message(const ChatMessage& msg) { messages_.push_back(msg); }

    const std::vector<ChatMessage>& messages() const { return messages_; }
    std::vector<ChatMessage>& messages_mut() { return messages_; }

    const std::string& cwd() const { return cwd_; }

    void set_context_window(int cw) { context_window_ = cw; }
    void set_no_model_config_prompt(std::string prompt) {
        no_model_config_prompt_ = std::move(prompt);
    }

    // Install / update the agent-loop termination policy. Called once from
    // main.cpp at startup (and could be called again if config reloads).
    // A fresh-default AgentLoopConfig is used when this setter is never called.
    void set_agent_loop_config(AgentLoopConfig cfg) { loop_cfg_ = cfg; }

    void set_session_manager(SessionManager* sm) { session_manager_ = sm; }
    void restore_goal_runtime();
    void publish_current_goal_state();
    void maybe_continue_goal();

    void set_skill_registry(const SkillRegistry* sr) { skill_registry_ = sr; }
    void set_memory_registry(const MemoryRegistry* mr) { memory_registry_ = mr; }
    void set_memory_config(const MemoryConfig* cfg) { memory_cfg_ = cfg; }
    void set_project_instructions_config(const ProjectInstructionsConfig* cfg) {
        project_instructions_cfg_ = cfg;
    }

    // ---- 事件流(Section 7 SessionClient)----
    // 老的 AgentCallbacks 路径**完全不动**:TUI 仍然用 callbacks。
    // SessionClient 走 events_,daemon HTTP/WebSocket handler 在 subscribe 上
    // 拿事件流。两者并行,不互相影响。
    EventDispatcher& events() { return events_; }

    // 注入异步 PermissionPrompter(daemon 模式)。不调用此 setter 时,AgentLoop
    // 默认走 callbacks_.on_tool_confirm 同步路径(TUI 模式)。线程安全要求:
    // 不在 worker 跑工具时调用 — 通常 SessionRegistry 创建 AgentLoop 后立刻
    // 调,然后才 submit 第一条消息。
    void set_permission_prompter(std::unique_ptr<PermissionPrompter> p) {
        prompter_ = std::move(p);
    }

    // 注入异步 AskUserQuestionPrompter(daemon 模式)。raw 指针;生命周期由
    // 调用方(典型是 SessionEntry)保证。AgentLoop 在每次工具调用前把它包成
    // ToolContext::ask_user_questions 回调注入。
    void set_ask_question_prompter(AskUserQuestionPrompter* p) {
        ask_prompter_ = p;
    }

private:
    void worker_main();
    void run_agent(const std::string& user_message);
    void run_agent_with_input(const UserInput& input,
                              bool hidden_goal_context = false);
    // Variant that records `display_text` into the user message's metadata.display_text
    // so UI can show the original input while the LLM sees an expanded `prompt`.
    // When `display_text` is empty, behaves identically to run_agent(prompt).
    void run_agent_with_display(const std::string& prompt,
                                const std::string& display_text,
                                bool hidden_goal_context = false);
    void run_shell(const std::string& command);
    void run_compact();
    void account_goal_usage(std::int64_t token_delta = 0, bool allow_complete = false);
    void emit_goal_updated(const ThreadGoal& goal);
    void emit_goal_cleared(const std::string& session_id);
    void emit_todo_updated(const nlohmann::json& payload);
    std::string build_goal_context_prompt(const ThreadGoal& goal) const;
    bool maybe_run_auto_compact();
    bool active_estimate_exceeds_auto_threshold() const;
    void apply_compact_result(const CompactResult& result);

    // Section 7: 同时调老 on_message callback(若 TUI 挂了)和新事件流
    // (events_)。所有 on_message 触发点都该走这个 helper,确保 daemon
    // 模式下没装 callbacks 也能拿到事件。
    void dispatch_message(const std::string& role,
                          const std::string& content,
                          bool is_tool,
                          nlohmann::json metadata = nlohmann::json::object(),
                          nlohmann::json content_parts = nlohmann::json::array());
    void append_turn_timing_record(const std::string& user_message_uuid,
                                   std::int64_t started_at_ms,
                                   std::int64_t completed_at_ms,
                                   const std::string& status);
    void append_tool_user_prompt(const std::string& content,
                                 const std::string& display_text,
                                 const std::string& source_tool);

    struct WorkerTask {
        enum class Kind { Chat, Shell, Compact };
        Kind kind = Kind::Chat;
        std::string payload;
        UserInput input;
        // 仅 Chat 用:UI 渲染时希望显示的"原文",而 payload(发给 LLM)可能
        // 是被 daemon expander 展开过的字符串(skill 调用提示等)。空 = UI 与
        // LLM 看到同一份(payload)。
        std::string display_text;
        bool hidden_goal_context = false;
    };

    ProviderAccessor provider_accessor_;
    ToolExecutor& tools_;
    AgentCallbacks callbacks_;
    std::vector<ChatMessage> messages_;
    std::atomic<bool> abort_requested_{false};
    std::atomic<bool> busy_{false};
    std::string cwd_;
    PermissionManager& permissions_;
    PathValidator path_validator_;
    int context_window_ = 128000;
    std::string no_model_config_prompt_;
    // agent_loop termination policy. Fresh defaults come from AgentLoopConfig
    // until set_agent_loop_config is called from main.cpp.
    AgentLoopConfig loop_cfg_;
    std::atomic<int> last_api_prompt_tokens_{0}; // from most recent API response
    int auto_compact_consecutive_failures_ = 0;
    SessionManager* session_manager_ = nullptr;
    const SkillRegistry* skill_registry_ = nullptr;
    const MemoryRegistry* memory_registry_ = nullptr;
    const MemoryConfig* memory_cfg_ = nullptr;
    const ProjectInstructionsConfig* project_instructions_cfg_ = nullptr;
    std::string session_context_cache_key_;
    std::string session_context_cache_content_;
    std::string goal_accounting_thread_id_;
    std::string goal_accounting_goal_id_;
    std::string budget_notice_goal_id_;
    std::chrono::steady_clock::time_point goal_time_checkpoint_{};
    std::map<std::string, std::chrono::steady_clock::time_point> recent_safe_edit_failures_;

    // Worker thread and task queue
    std::thread worker_thread_;
    std::mutex queue_mu_;
    std::condition_variable queue_cv_;
    std::queue<WorkerTask> task_queue_;
    bool shutdown_requested_ = false;

    // Section 7: 事件分发器。EventDispatcher 自己内部加锁,所以这里不需要
    // 额外的同步;emit 由 worker_main 线程调用,subscribe/unsubscribe 由
    // HTTP handler 线程并发调用。
    EventDispatcher events_;

    // Section 7.6: PermissionPrompter。null 时走 callbacks_.on_tool_confirm
    // 老路径(TUI);非 null 时(daemon 模式)走 prompter_->prompt。
    std::unique_ptr<PermissionPrompter> prompter_;

    // AskUserQuestionPrompter: daemon 模式下走 WS。raw 指针,生命周期由
    // SessionEntry 持有。null 时 ToolContext::ask_user_questions 不注入,
    // 此时 AskUserQuestion 工具(daemon 工厂版)会返回 rejected。
    AskUserQuestionPrompter* ask_prompter_ = nullptr;
};

} // namespace acecode
