#pragma once

#include "provider/llm_provider.hpp"
#include "tool/tool_executor.hpp"
#include "permissions.hpp"
#include "utils/path_validator.hpp"
#include "utils/token_tracker.hpp"
#include "session/session_manager.hpp"

#include <vector>
#include <string>
#include <functional>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <queue>

namespace acecode {

class SkillRegistry;
class MemoryRegistry;
struct MemoryConfig;
struct ProjectInstructionsConfig;

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

    // Called when auto-compact is needed (estimated tokens exceed threshold)
    // Returns true if compaction was performed successfully
    std::function<bool()> on_auto_compact;

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

    // Submit a user-initiated shell command triggered by `!` mode. Non-blocking:
    // enqueues on the same worker so it serialises with LLM turns. The worker
    // invokes BashTool directly (no LLM round-trip), emits tool_call + tool_result
    // UI messages via callbacks, and appends a `<bash-input>/<bash-stdout>/...`
    // user-role entry to messages_ for the next LLM turn.
    void submit_shell(std::string command);

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

    // Signal the worker thread to exit and wait for it to finish.
    void shutdown();

    // Returns true if abort has been requested. Useful for confirm callbacks.
    bool is_aborting() const { return abort_requested_.load(); }

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

    void set_session_manager(SessionManager* sm) { session_manager_ = sm; }

    void set_skill_registry(const SkillRegistry* sr) { skill_registry_ = sr; }
    void set_memory_registry(const MemoryRegistry* mr) { memory_registry_ = mr; }
    void set_memory_config(const MemoryConfig* cfg) { memory_cfg_ = cfg; }
    void set_project_instructions_config(const ProjectInstructionsConfig* cfg) {
        project_instructions_cfg_ = cfg;
    }

private:
    void worker_main();
    void run_agent(const std::string& user_message);
    void run_shell(const std::string& command);

    struct WorkerTask {
        enum class Kind { Chat, Shell };
        Kind kind = Kind::Chat;
        std::string payload;
    };

    ProviderAccessor provider_accessor_;
    ToolExecutor& tools_;
    AgentCallbacks callbacks_;
    std::vector<ChatMessage> messages_;
    std::atomic<bool> abort_requested_{false};
    std::string cwd_;
    PermissionManager& permissions_;
    PathValidator path_validator_;
    int context_window_ = 128000;
    std::atomic<int> last_api_prompt_tokens_{0}; // from most recent API response
    SessionManager* session_manager_ = nullptr;
    const SkillRegistry* skill_registry_ = nullptr;
    const MemoryRegistry* memory_registry_ = nullptr;
    const MemoryConfig* memory_cfg_ = nullptr;
    const ProjectInstructionsConfig* project_instructions_cfg_ = nullptr;

    // Worker thread and task queue
    std::thread worker_thread_;
    std::mutex queue_mu_;
    std::condition_variable queue_cv_;
    std::queue<WorkerTask> task_queue_;
    bool shutdown_requested_ = false;
};

} // namespace acecode
