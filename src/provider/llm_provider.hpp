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

    // Chain-of-thought emitted by reasoning-mode LLMs (DeepSeek thinking,
    // Qwen, Moonshot, OpenRouter, …). DeepSeek requires the previous turn's
    // reasoning_content to be echoed back on assistant messages — see
    // openspec/changes/support-deepseek-reasoning. Empty for non-reasoning
    // models; never set on user / system / tool messages.
    std::string reasoning_content;

    // Metadata fields for compact pipeline
    std::string uuid;                    // unique identifier (for boundary tracking)
    std::string subtype;                 // "compact_boundary" | "microcompact_boundary" | ""
    std::string timestamp;               // ISO 8601 timestamp
    bool is_meta = false;                // meta-message (boundary etc.), not sent to API
    bool is_compact_summary = false;     // marks this message as a compact summary
    nlohmann::json metadata;             // extended metadata (compact stats etc.)

    // Runtime-only compact preview for TUI rendering of tool_call rows. Not
    // serialized to session JSONL. When empty, the TUI falls back to the
    // legacy `[Tool: X] {JSON}` format.
    std::string display_override;
};

struct ToolCall {
    std::string id;
    std::string function_name;
    std::string function_arguments; // raw JSON string
};

struct TokenUsage {
    int prompt_tokens = 0;
    int completion_tokens = 0;
    int total_tokens = 0;
    int cache_read_tokens = 0;   // from prompt_tokens_details.cached_tokens
    int cache_write_tokens = 0;  // from prompt_tokens_details.cache_write_tokens
    int reasoning_tokens = 0;    // from completion_tokens_details.reasoning_tokens
    bool has_data = false; // true if server returned usage info
};

struct ChatResponse {
    std::string content;               // text reply (empty if tool_calls present)
    std::string reasoning_content;     // chain-of-thought (DeepSeek thinking etc.)
    std::vector<ToolCall> tool_calls;  // empty if pure text reply
    std::string finish_reason;         // "stop", "tool_calls", etc.
    TokenUsage usage;

    bool has_tool_calls() const { return !tool_calls.empty(); }
};

struct ToolDef {
    std::string name;
    std::string description;
    nlohmann::json parameters; // JSON Schema object
};

// Streaming event types for chat_stream()
//   ReasoningDelta — chain-of-thought fragment from a reasoning-mode model.
//   Callbacks are free to ignore it; today the agent loop drops it silently
//   and a future TUI panel can subscribe.
enum class StreamEventType { Delta, ToolCall, Done, Error, Usage, ReasoningDelta };

struct StreamEvent {
    StreamEventType type;
    std::string content;        // Delta: token fragment
    ToolCall tool_call;         // ToolCall: complete tool call
    std::string error;          // Error: description
    TokenUsage usage;           // Usage: token counts from server
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

    virtual std::string model() const = 0;
    virtual void set_model(const std::string& m) = 0;

    virtual bool authenticate() { return true; }
};

} // namespace acecode
