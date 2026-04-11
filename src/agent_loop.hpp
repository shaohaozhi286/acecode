#pragma once

#include "provider/llm_provider.hpp"
#include "tool/tool_executor.hpp"

#include <vector>
#include <string>
#include <functional>
#include <mutex>
#include <atomic>

namespace acecode {

// Callbacks for the TUI to observe agent loop events
struct AgentCallbacks {
    // Called when a new message is added to the conversation
    std::function<void(const std::string& role, const std::string& content, bool is_tool)> on_message;

    // Called when the agent starts/stops processing
    std::function<void(bool busy)> on_busy_changed;

    // Called to request user confirmation for a tool call.
    // Returns true if user approves, false if denied.
    std::function<bool(const std::string& tool_name, const std::string& arguments)> on_tool_confirm;

    // Called for each streaming delta token (real-time TUI update)
    std::function<void(const std::string& token)> on_delta;
};

class AgentLoop {
public:
    AgentLoop(LlmProvider& provider, ToolExecutor& tools, AgentCallbacks callbacks,
              const std::string& cwd);

    void set_callbacks(AgentCallbacks cb);

    // Submit a user message and run the agent loop until a final text response.
    // This runs synchronously and should be called from a background thread.
    void submit(const std::string& user_message);

    // Abort the current inference. Safe to call from any thread.
    void abort();

    // Legacy cancel alias
    void cancel() { abort(); }

    const std::vector<ChatMessage>& messages() const { return messages_; }

private:
    LlmProvider& provider_;
    ToolExecutor& tools_;
    AgentCallbacks callbacks_;
    std::vector<ChatMessage> messages_;
    std::atomic<bool> abort_requested_{false};
    std::string cwd_;
};

} // namespace acecode
