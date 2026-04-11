#pragma once

#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <nlohmann/json.hpp>

namespace acecode {

struct ChatMessage {
    std::string role;    // "system", "user", "assistant", "tool"
    std::string content;

    // For assistant messages with tool calls
    nlohmann::json tool_calls; // array of tool_call objects, empty if none

    // For tool result messages
    std::string tool_call_id;
};

struct ToolCall {
    std::string id;
    std::string function_name;
    std::string function_arguments; // raw JSON string
};

struct ChatResponse {
    std::string content;               // text reply (empty if tool_calls present)
    std::vector<ToolCall> tool_calls;  // empty if pure text reply
    std::string finish_reason;         // "stop", "tool_calls", etc.

    bool has_tool_calls() const { return !tool_calls.empty(); }
};

struct ToolDef {
    std::string name;
    std::string description;
    nlohmann::json parameters; // JSON Schema object
};

// Streaming event types for chat_stream()
enum class StreamEventType { Delta, ToolCall, Done, Error };

struct StreamEvent {
    StreamEventType type;
    std::string content;        // Delta: token fragment
    ToolCall tool_call;         // ToolCall: complete tool call
    std::string error;          // Error: description
};

using StreamCallback = std::function<void(const StreamEvent&)>;

class LlmProvider {
public:
    virtual ~LlmProvider() = default;

    virtual ChatResponse chat(
        const std::vector<ChatMessage>& messages,
        const std::vector<ToolDef>& tools
    ) = 0;

    // Streaming chat: invokes callback for each event. abort_flag can cancel the request.
    virtual void chat_stream(
        const std::vector<ChatMessage>& messages,
        const std::vector<ToolDef>& tools,
        const StreamCallback& callback,
        std::atomic<bool>* abort_flag = nullptr
    ) = 0;

    virtual std::string name() const = 0;
    virtual bool is_authenticated() = 0;

    virtual bool authenticate() { return true; }
};

} // namespace acecode
