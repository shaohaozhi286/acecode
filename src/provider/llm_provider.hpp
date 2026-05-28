#pragma once

#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <cstddef>
#include <nlohmann/json.hpp>

namespace acecode {

struct ChatMessage {
    std::string role;    // "system", "user", "assistant", "tool"
    std::string content;
    nlohmann::json content_parts; // neutral structured text/image/file/context parts

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

struct UserInput {
    std::string text;
    std::string display_text;
    nlohmann::json content_parts = nlohmann::json::array();
    nlohmann::json metadata = nlohmann::json::object();

    bool has_content_parts() const {
        return !content_parts.is_null() && content_parts.is_array() && !content_parts.empty();
    }

    bool empty() const {
        return text.empty() && !has_content_parts();
    }
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

enum class ProviderErrorKind {
    None,
    UserCancelled,
    Timeout,
    Network,
    Http,
    MalformedSse,
    MalformedJson,
    Unknown,
};

struct ProviderErrorInfo {
    ProviderErrorKind kind = ProviderErrorKind::None;
    int status_code = 0;
    std::string provider;
    std::string model;
    std::string request_id;
    std::string display_message;
    std::string raw_body;
    bool body_is_json = false;
    std::string pretty_json;
    bool retryable = false;
    int retry_attempt = 0;
    int retry_max_attempts = 0;
    int retry_delay_ms = 0;

    bool has_error() const { return kind != ProviderErrorKind::None; }
};

// Streaming event types for chat_stream()
//   ReasoningDelta — chain-of-thought fragment from a reasoning-mode model.
//   ToolCallDelta — safe metadata while a streaming provider is still
//                   accumulating a tool call; tool_call.function_arguments is
//                   partial and should not be rendered raw to users.
//   Callbacks are free to ignore it; today the agent loop drops it silently
//   and a future TUI panel can subscribe.
//   Retry — provider is retrying a transient failure. Timeout retries may occur
//           after provisional stream output; consumers should discard partial
//           assistant/tool state before accepting output from the next attempt.
enum class StreamEventType {
    Delta,
    ToolCall,
    ToolCallDelta,
    Done,
    Error,
    Usage,
    Retry,
    ReasoningDelta,
};

struct StreamEvent {
    StreamEventType type;
    std::string content;        // Delta: token fragment
    ToolCall tool_call;         // ToolCall: complete call; ToolCallDelta: partial metadata
    int tool_index = -1;        // ToolCall/ToolCallDelta: index within current assistant turn
    std::size_t tool_call_argument_bytes = 0; // ToolCallDelta: accumulated argument bytes
    std::string error;          // Error: description
    ProviderErrorInfo provider_error; // Error/Retry: structured provider failure
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
