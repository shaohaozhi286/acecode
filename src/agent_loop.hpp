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

// Callbacks for the TUI to observe agent loop events
struct AgentCallbacks {
    // Called when a new message is added to the conversation
    std::function<void(const std::string& role, const std::string& content, bool is_tool)> on_message;

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
};

class AgentLoop {
public:
    AgentLoop(LlmProvider& provider, ToolExecutor& tools, AgentCallbacks callbacks,
              const std::string& cwd, PermissionManager& permissions);
    ~AgentLoop();

    void set_callbacks(AgentCallbacks cb);

    // Submit a user message. Non-blocking: enqueues the message and returns immediately.
    // The internal worker thread will process it.
    void submit(const std::string& user_message);

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

private:
    void worker_main();
    void run_agent(const std::string& user_message);

    LlmProvider& provider_;
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

    // Worker thread and task queue
    std::thread worker_thread_;
    std::mutex queue_mu_;
    std::condition_variable queue_cv_;
    std::queue<std::string> task_queue_;
    bool shutdown_requested_ = false;
};

} // namespace acecode
