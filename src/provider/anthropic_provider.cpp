#include "anthropic_provider.hpp"

#include "config/request_headers.hpp"
#include "network/proxy_resolver.hpp"
#include "utils/logger.hpp"
#include "utils/sha1.hpp"

#include <cpr/cpr.h>
#include <cpr/ssl_options.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <map>
#include <optional>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace acecode {

AnthropicProvider::AnthropicProvider(const std::string& base_url,
                                     const std::string& api_key,
                                     const std::string& model,
                                     int stream_timeout_ms,
                                     std::map<std::string, std::string> request_headers)
    : base_url_(normalize_base_url(base_url)),
      api_key_(api_key),
      model_(model),
      request_headers_(std::move(request_headers)),
      stream_timeout_ms_(stream_timeout_ms > 0
          ? stream_timeout_ms
          : OpenAiConfig::kDefaultStreamTimeoutMs) {}

namespace {

constexpr int kStreamMaxAttempts = 3;
constexpr int kStreamRetryBaseDelayMs = 100;
constexpr int kStreamRetryMaxDelayMs = 15000;
constexpr int kStreamRetrySleepSliceMs = 50;
constexpr int kStreamConnectTimeoutCapMs = 15000;

std::int64_t steady_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string json_string_or_empty(const nlohmann::json& value, const char* key) {
    if (!value.is_object() || !value.contains(key) || !value[key].is_string()) {
        return {};
    }
    return value[key].get<std::string>();
}

std::string synthesize_tool_call_id(int index,
                                    const std::string& name,
                                    const std::string& arguments) {
    std::string fingerprint = std::to_string(index);
    fingerprint.push_back('\n');
    fingerprint.append(name);
    fingerprint.push_back('\n');
    fingerprint.append(arguments);
    return "toolu_ace_" + sha1_hex(fingerprint).substr(0, 24);
}

nlohmann::json parse_tool_input_or_empty(const std::string& arguments) {
    auto parsed = nlohmann::json::parse(arguments, nullptr, false);
    if (!parsed.is_discarded() && (parsed.is_object() || parsed.is_array())) {
        return parsed;
    }
    return nlohmann::json::object();
}

void append_text_block(nlohmann::json& blocks, const std::string& text) {
    if (text.empty()) return;
    blocks.push_back(nlohmann::json{{"type", "text"}, {"text", text}});
}

std::string textual_content_parts(const ChatMessage& msg) {
    if (msg.content_parts.is_null() || !msg.content_parts.is_array() ||
        msg.content_parts.empty()) {
        return msg.content;
    }

    std::ostringstream oss;
    bool first = true;
    auto append = [&](const std::string& text) {
        if (text.empty()) return;
        if (!first) oss << "\n\n";
        first = false;
        oss << text;
    };

    for (const auto& part : msg.content_parts) {
        if (!part.is_object()) continue;
        const std::string type = part.value("type", std::string{});
        if (type == "text") {
            append(part.value("text", std::string{}));
        } else if (type == "browser_context") {
            const auto ctx = part.contains("context") ? part["context"] : nlohmann::json::object();
            append("[Browser context]\n" + ctx.dump(2));
        } else if (type == "file") {
            append("[Attached file omitted: Anthropic file blocks are not supported by this provider yet]");
        } else if (type == "image") {
            append("[Image attachment omitted: Anthropic image blocks are not supported by this provider yet]");
        } else if (!part.is_null()) {
            append(part.dump());
        }
    }

    std::string text = oss.str();
    if (text.empty()) return msg.content;
    return text;
}

nlohmann::json anthropic_content_blocks_for_text(const std::string& text) {
    nlohmann::json blocks = nlohmann::json::array();
    append_text_block(blocks, text);
    return blocks;
}

nlohmann::json anthropic_tool_use_blocks(const nlohmann::json& tool_calls,
                                         int& repaired_count,
                                         int& dropped_count) {
    nlohmann::json items;
    if (tool_calls.is_array()) {
        items = tool_calls;
    } else if (tool_calls.is_object()) {
        items = nlohmann::json::array({tool_calls});
    } else {
        return nlohmann::json::array();
    }

    nlohmann::json out = nlohmann::json::array();
    int index = 0;
    for (const auto& raw_tc : items) {
        if (!raw_tc.is_object()) {
            ++dropped_count;
            continue;
        }
        std::string id = json_string_or_empty(raw_tc, "id");
        if (!raw_tc.contains("function") || !raw_tc["function"].is_object()) {
            ++dropped_count;
            continue;
        }
        const auto& fn = raw_tc["function"];
        std::string name = json_string_or_empty(fn, "name");
        std::string arguments = json_string_or_empty(fn, "arguments");
        if (name.empty()) {
            ++dropped_count;
            continue;
        }
        if (id.empty()) {
            id = synthesize_tool_call_id(index, name, arguments);
            ++repaired_count;
        }
        out.push_back(nlohmann::json{
            {"type", "tool_use"},
            {"id", id},
            {"name", name},
            {"input", parse_tool_input_or_empty(arguments)},
        });
        ++index;
    }
    return out;
}

void merge_usage(TokenUsage& usage, const nlohmann::json& node) {
    if (!node.is_object()) return;
    if (node.contains("input_tokens") && node["input_tokens"].is_number_integer()) {
        usage.prompt_tokens = node["input_tokens"].get<int>();
        usage.has_data = true;
    }
    if (node.contains("output_tokens") && node["output_tokens"].is_number_integer()) {
        usage.completion_tokens = node["output_tokens"].get<int>();
        usage.has_data = true;
    }
    if (node.contains("cache_read_input_tokens") &&
        node["cache_read_input_tokens"].is_number_integer()) {
        usage.cache_read_tokens = node["cache_read_input_tokens"].get<int>();
        usage.has_data = true;
    }
    if (node.contains("cache_creation_input_tokens") &&
        node["cache_creation_input_tokens"].is_number_integer()) {
        usage.cache_write_tokens = node["cache_creation_input_tokens"].get<int>();
        usage.has_data = true;
    }
    if (usage.has_data) {
        usage.total_tokens = usage.prompt_tokens + usage.completion_tokens;
    }
}

std::string header_value_ci(const cpr::Header& headers, const std::string& key) {
    const std::string wanted = ascii_lower(key);
    for (const auto& [header_key, value] : headers) {
        if (ascii_lower(header_key) == wanted) return value;
    }
    return {};
}

std::string extract_request_id(const cpr::Header& headers) {
    for (const std::string& key : {
             "request-id",
             "x-request-id",
             "cf-ray",
         }) {
        std::string value = header_value_ci(headers, key);
        if (!value.empty()) return value;
    }
    return {};
}

bool parse_json_body(const std::string& body,
                     bool& body_is_json,
                     std::string& pretty_json) {
    body_is_json = false;
    pretty_json.clear();
    if (body.empty()) return false;
    try {
        const auto parsed = nlohmann::json::parse(body);
        pretty_json = parsed.dump(2);
        body_is_json = true;
        return true;
    } catch (const nlohmann::json::parse_error&) {
        return false;
    }
}

std::string provider_error_prefix(ProviderErrorKind kind, int status_code) {
    switch (kind) {
    case ProviderErrorKind::UserCancelled: return "Request cancelled";
    case ProviderErrorKind::Timeout:       return "Request timed out";
    case ProviderErrorKind::Network:       return "Connection failed";
    case ProviderErrorKind::Http:          return "HTTP " + std::to_string(status_code);
    case ProviderErrorKind::MalformedJson: return "Malformed JSON in streaming response";
    case ProviderErrorKind::MalformedSse:  return "Malformed or incomplete streaming response";
    case ProviderErrorKind::Unknown:       return "Provider request failed";
    case ProviderErrorKind::None:          return {};
    }
    return "Provider request failed";
}

ProviderErrorInfo make_provider_error(ProviderErrorKind kind,
                                      int status_code,
                                      const std::string& provider,
                                      const std::string& model,
                                      const std::string& request_id,
                                      const std::string& raw_body,
                                      const std::string& transport_message,
                                      bool retryable) {
    ProviderErrorInfo info;
    info.kind = kind;
    info.status_code = status_code;
    info.provider = provider;
    info.model = model;
    info.request_id = request_id;
    info.raw_body = raw_body;
    info.retryable = retryable;
    parse_json_body(raw_body, info.body_is_json, info.pretty_json);

    if (kind == ProviderErrorKind::UserCancelled) {
        info.display_message = "Request cancelled";
        return info;
    }

    std::ostringstream display;
    display << provider_error_prefix(kind, status_code);
    if (!provider.empty()) display << " from " << provider;
    if (!model.empty()) display << " model " << model;
    if (!request_id.empty()) display << " request_id=" << request_id;
    if (!transport_message.empty()) display << ": " << transport_message;
    const std::string body_for_display = info.body_is_json ? info.pretty_json : raw_body;
    if (!body_for_display.empty()) {
        display << "\n" << body_for_display;
    }
    info.display_message = display.str();
    return info;
}

ChatResponse make_chat_error_response(ProviderErrorInfo info) {
    ChatResponse response;
    response.content = "[Error] " + info.display_message;
    response.finish_reason = "error";
    response.provider_error = std::move(info);
    return response;
}

ProviderErrorKind classify_cpr_error(const cpr::Error& error) {
    if (error.code == cpr::ErrorCode::OPERATION_TIMEDOUT) {
        return ProviderErrorKind::Timeout;
    }
    const std::string message = ascii_lower(error.message);
    if (message.find("timed out") != std::string::npos ||
        message.find("timeout") != std::string::npos) {
        return ProviderErrorKind::Timeout;
    }
    return ProviderErrorKind::Network;
}

bool is_retryable_http_status(int status_code, const std::string& body) {
    if (status_code == 408 || status_code == 409 || status_code == 429 || status_code == 529) {
        return true;
    }
    if (status_code >= 500 && status_code < 600) return true;
    const std::string lower_body = ascii_lower(body);
    return lower_body.find("overloaded_error") != std::string::npos ||
           lower_body.find("overloaded") != std::string::npos;
}

void emit_provider_error(const StreamCallback& callback, const ProviderErrorInfo& info) {
    StreamEvent evt;
    evt.type = StreamEventType::Error;
    evt.error = info.display_message;
    evt.provider_error = info;
    callback(evt);
}

void emit_retry_event(const StreamCallback& callback, ProviderErrorInfo info) {
    StreamEvent evt;
    evt.type = StreamEventType::Retry;
    evt.provider_error = std::move(info);
    evt.error = evt.provider_error.display_message;
    callback(evt);
}

bool sleep_retry_or_aborted(int delay_ms, std::atomic<bool>* abort_flag) {
    int slept_ms = 0;
    while (slept_ms < delay_ms) {
        if (abort_flag && abort_flag->load()) return true;
        const int slice = (std::min)(kStreamRetrySleepSliceMs, delay_ms - slept_ms);
        std::this_thread::sleep_for(std::chrono::milliseconds(slice));
        slept_ms += slice;
    }
    return abort_flag && abort_flag->load();
}

int retry_after_delay_ms(const cpr::Header& headers, int attempt_index) {
    int exponential = kStreamRetryBaseDelayMs;
    const int capped_attempt = (std::min)(attempt_index, 16);
    for (int i = 1; i < capped_attempt; ++i) {
        exponential = (std::min)(kStreamRetryMaxDelayMs, exponential * 2);
    }

    const std::string retry_after = header_value_ci(headers, "retry-after");
    if (retry_after.empty()) return exponential;

    try {
        const double seconds = std::stod(retry_after);
        if (seconds < 0.0) return exponential;
        const int retry_after_ms = static_cast<int>(seconds * 1000.0);
        return (std::min)(kStreamRetryMaxDelayMs, (std::max)(0, retry_after_ms));
    } catch (...) {
        return exponential;
    }
}

bool find_event_delimiter(const std::string& buffer,
                          size_t& pos,
                          size_t& delimiter_len) {
    const size_t lf = buffer.find("\n\n");
    const size_t crlf = buffer.find("\r\n\r\n");
    if (lf == std::string::npos && crlf == std::string::npos) return false;
    if (crlf != std::string::npos &&
        (lf == std::string::npos || crlf < lf)) {
        pos = crlf;
        delimiter_len = 4;
    } else {
        pos = lf;
        delimiter_len = 2;
    }
    return true;
}

struct SseEventBlock {
    std::string event;
    std::string data;
};

SseEventBlock parse_sse_event_block(const std::string& event_block) {
    SseEventBlock out;
    std::istringstream iss(event_block);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        static constexpr std::string_view kEventPrefix = "event:";
        static constexpr std::string_view kDataPrefix = "data:";
        if (line.compare(0, kEventPrefix.size(), kEventPrefix) == 0) {
            size_t value_start = kEventPrefix.size();
            if (value_start < line.size() && line[value_start] == ' ') ++value_start;
            out.event = line.substr(value_start);
        } else if (line.compare(0, kDataPrefix.size(), kDataPrefix) == 0) {
            size_t value_start = kDataPrefix.size();
            if (value_start < line.size() && line[value_start] == ' ') ++value_start;
            if (!out.data.empty()) out.data += "\n";
            out.data += line.substr(value_start);
        }
    }
    return out;
}

std::string anthropic_error_message_from_json(const nlohmann::json& j) {
    if (!j.is_object()) return {};
    auto it = j.find("error");
    if (it == j.end()) return {};
    const auto& error = *it;
    if (error.is_string()) return error.get<std::string>();
    if (error.is_object()) {
        const std::string type = error.value("type", std::string{});
        const std::string message = error.value("message", std::string{});
        if (!type.empty() && !message.empty()) return type + ": " + message;
        if (!message.empty()) return message;
        if (!type.empty()) return type;
    }
    return error.dump();
}

} // namespace

nlohmann::json AnthropicProvider::build_request_body(
    const std::vector<ChatMessage>& messages,
    const std::vector<ToolDef>& tools,
    bool stream
) const {
    nlohmann::json body;
    body["model"] = model_;
    body["max_tokens"] = kDefaultMaxTokens;
    if (stream) body["stream"] = true;

    std::string system_text;
    nlohmann::json anthropic_messages = nlohmann::json::array();
    int repaired_tool_calls = 0;
    int dropped_tool_calls = 0;
    int dropped_invalid_role = 0;

    for (const auto& msg : messages) {
        if (msg.role == "system") {
            const std::string text = textual_content_parts(msg);
            if (!text.empty()) {
                if (!system_text.empty()) system_text += "\n\n";
                system_text += text;
            }
            continue;
        }

        if (msg.role == "tool") {
            if (msg.tool_call_id.empty()) {
                nlohmann::json blocks = anthropic_content_blocks_for_text(
                    "[Tool result omitted: missing tool_use_id]\n" + msg.content);
                anthropic_messages.push_back(nlohmann::json{
                    {"role", "user"},
                    {"content", std::move(blocks)},
                });
                continue;
            }
            nlohmann::json blocks = nlohmann::json::array();
            blocks.push_back(nlohmann::json{
                {"type", "tool_result"},
                {"tool_use_id", msg.tool_call_id},
                {"content", textual_content_parts(msg)},
            });
            anthropic_messages.push_back(nlohmann::json{
                {"role", "user"},
                {"content", std::move(blocks)},
            });
            continue;
        }

        if (msg.role != "user" && msg.role != "assistant") {
            ++dropped_invalid_role;
            continue;
        }

        nlohmann::json blocks = nlohmann::json::array();
        const std::string text = textual_content_parts(msg);
        append_text_block(blocks, text);
        if (msg.role == "assistant" && !msg.reasoning_content.empty()) {
            append_text_block(blocks, "[Previous reasoning]\n" + msg.reasoning_content);
        }
        if (msg.role == "assistant" && !msg.tool_calls.is_null() && !msg.tool_calls.empty()) {
            nlohmann::json tool_blocks = anthropic_tool_use_blocks(
                msg.tool_calls, repaired_tool_calls, dropped_tool_calls);
            for (auto& block : tool_blocks) blocks.push_back(std::move(block));
        }
        if (blocks.empty()) {
            append_text_block(blocks, "[Empty message]");
        }

        anthropic_messages.push_back(nlohmann::json{
            {"role", msg.role},
            {"content", std::move(blocks)},
        });
    }

    if (!system_text.empty()) body["system"] = system_text;
    body["messages"] = std::move(anthropic_messages);

    if (!tools.empty()) {
        nlohmann::json tools_json = nlohmann::json::array();
        for (const auto& tool : tools) {
            nlohmann::json t;
            t["name"] = tool.name;
            t["description"] = tool.description;
            t["input_schema"] = tool.parameters.is_object()
                ? tool.parameters
                : nlohmann::json::object();
            tools_json.push_back(std::move(t));
        }
        body["tools"] = std::move(tools_json);
    }

    if (repaired_tool_calls > 0) {
        LOG_WARN("anthropic build_request_body: synthesized " +
                 std::to_string(repaired_tool_calls) + " tool_use id(s)");
    }
    if (dropped_tool_calls > 0 || dropped_invalid_role > 0) {
        LOG_WARN("anthropic build_request_body: dropped invalid entries roles=" +
                 std::to_string(dropped_invalid_role) + " tool_calls=" +
                 std::to_string(dropped_tool_calls));
    }

    return body;
}

ChatResponse AnthropicProvider::parse_response(const nlohmann::json& j) {
    ChatResponse resp;
    resp.finish_reason = j.value("stop_reason", std::string{"stop"});
    if (j.contains("usage")) merge_usage(resp.usage, j["usage"]);

    if (!j.contains("content") || !j["content"].is_array()) {
        resp.content = "[Error] No content blocks in Anthropic response.";
        resp.finish_reason = "error";
        return resp;
    }

    int tool_index = 0;
    resp.content_parts = nlohmann::json::array();
    for (const auto& block : j["content"]) {
        if (!block.is_object()) continue;
        resp.content_parts.push_back(block);
        const std::string type = block.value("type", std::string{});
        if (type == "text" && block.contains("text") && block["text"].is_string()) {
            resp.content += block["text"].get<std::string>();
        } else if (type == "thinking" && block.contains("thinking") &&
                   block["thinking"].is_string()) {
            resp.reasoning_content += block["thinking"].get<std::string>();
        } else if (type == "tool_use") {
            ToolCall call;
            call.id = block.value("id", std::string{});
            call.function_name = block.value("name", std::string{});
            if (block.contains("input")) {
                call.function_arguments = block["input"].dump();
            } else {
                call.function_arguments = "{}";
            }
            if (call.id.empty()) {
                call.id = synthesize_tool_call_id(
                    tool_index, call.function_name, call.function_arguments);
            }
            resp.tool_calls.push_back(std::move(call));
            ++tool_index;
        }
    }
    return resp;
}

ChatResponse AnthropicProvider::chat(
    const std::vector<ChatMessage>& messages,
    const std::vector<ToolDef>& tools
) {
    if (api_key_.empty()) {
        auto info = make_provider_error(
            ProviderErrorKind::Unknown,
            0,
            name(),
            model_,
            std::string{},
            std::string{},
            "missing Anthropic API key",
            false);
        return make_chat_error_response(std::move(info));
    }

    nlohmann::json body = build_request_body(messages, tools, false);
    const std::string url = base_url_ + "/messages";

    cpr::Header headers = {
        {"Content-Type", "application/json"},
        {"anthropic-version", "2023-06-01"},
        {"x-api-key", api_key_},
    };
    std::string header_error;
    auto resolved_headers = resolve_request_headers(request_headers_, header_error);
    if (!resolved_headers.has_value()) {
        return make_chat_error_response(make_provider_error(
            ProviderErrorKind::Unknown,
            0,
            name(),
            model_,
            std::string{},
            std::string{},
            header_error,
            false));
    }
    for (const auto& [k, v] : *resolved_headers) {
        headers[k] = v;
    }

    auto proxy_opts = network::proxy_options_for(url);
    cpr::Response r = cpr::Post(
        cpr::Url{url},
        headers,
        cpr::Body{body.dump()},
        network::build_ssl_options(proxy_opts),
        proxy_opts.proxies,
        proxy_opts.auth,
        cpr::Timeout{stream_timeout_ms_}
    );

    if (r.status_code == 0) {
        const ProviderErrorKind kind = classify_cpr_error(r.error);
        return make_chat_error_response(make_provider_error(
            kind,
            0,
            name(),
            model_,
            extract_request_id(r.header),
            r.text,
            r.error.message,
            kind == ProviderErrorKind::Timeout ||
                kind == ProviderErrorKind::Network));
    }

    if (r.status_code < 200 || r.status_code >= 300) {
        return make_chat_error_response(make_provider_error(
            ProviderErrorKind::Http,
            static_cast<int>(r.status_code),
            name(),
            model_,
            extract_request_id(r.header),
            r.text,
            std::string{},
            is_retryable_http_status(static_cast<int>(r.status_code), r.text)));
    }

    try {
        nlohmann::json response_json = nlohmann::json::parse(r.text);
        return parse_response(response_json);
    } catch (const nlohmann::json::parse_error& e) {
        return make_chat_error_response(make_provider_error(
            ProviderErrorKind::MalformedJson,
            200,
            name(),
            model_,
            extract_request_id(r.header),
            r.text,
            e.what(),
            false));
    }
}

ChatResponse AnthropicProvider::parse_sse_stream(
    const std::string& url,
    const nlohmann::json& body,
    const std::map<std::string, std::string>& extra_headers,
    const StreamCallback& callback,
    std::atomic<bool>* abort_flag
) {
    LOG_INFO("anthropic parse_sse_stream url=" + url);

    cpr::Header headers = {
        {"Content-Type", "application/json"},
        {"anthropic-version", "2023-06-01"},
    };
    for (const auto& [k, v] : extra_headers) {
        headers[k] = v;
    }

    std::atomic<std::int64_t> last_stream_activity_ms{steady_now_ms()};
    std::atomic<bool> stream_idle_timed_out{false};
    const int stream_idle_timeout_ms = (std::max)(1, stream_timeout_ms_);

    auto progress_cb = cpr::ProgressCallback{
        [abort_flag, &last_stream_activity_ms, &stream_idle_timed_out,
         stream_idle_timeout_ms](cpr::cpr_off_t,
                                 cpr::cpr_off_t,
                                 cpr::cpr_off_t,
                                 cpr::cpr_off_t,
                                 intptr_t) -> bool {
            if (abort_flag && abort_flag->load()) {
                return false;
            }
            const std::int64_t idle_ms =
                steady_now_ms() - last_stream_activity_ms.load();
            if (idle_ms >= stream_idle_timeout_ms) {
                stream_idle_timed_out.store(true);
                return false;
            }
            return true;
        }
    };

    auto proxy_opts = network::proxy_options_for(url);

    struct ContentBlockAccumulator {
        std::string type;
        std::string id;
        std::string name;
        std::string input_json;
    };

    ChatResponse last_accumulated;
    last_accumulated.finish_reason = "stop";

    for (int attempt = 1; ; ++attempt) {
        last_stream_activity_ms.store(steady_now_ms());
        stream_idle_timed_out.store(false);

        ChatResponse accumulated;
        accumulated.finish_reason = "stop";
        std::string reported_finish_reason;
        std::string sse_buffer;
        std::string raw_body_capture;
        std::map<int, ContentBlockAccumulator> blocks;
        bool saw_done = false;
        bool saw_sse_data = false;
        bool saw_parse_error = false;
        bool saw_payload_error = false;
        std::string payload_error_body;
        std::string payload_error_message;
        bool emitted_stream_output = false;

        auto emit_usage = [&]() {
            if (!accumulated.usage.has_data) return;
            StreamEvent usage_evt;
            usage_evt.type = StreamEventType::Usage;
            usage_evt.usage = accumulated.usage;
            callback(usage_evt);
        };

        auto flush_tool_block = [&](int index) {
            auto it = blocks.find(index);
            if (it == blocks.end() || it->second.type != "tool_use") return;
            const auto& block = it->second;
            ToolCall call;
            call.id = block.id.empty()
                ? synthesize_tool_call_id(index, block.name, block.input_json)
                : block.id;
            call.function_name = block.name;
            call.function_arguments = block.input_json.empty()
                ? std::string("{}")
                : block.input_json;
            accumulated.tool_calls.push_back(call);

            StreamEvent evt;
            evt.type = StreamEventType::ToolCall;
            evt.tool_call = call;
            evt.tool_index = index;
            callback(evt);
            emitted_stream_output = true;
        };

        auto emit_done = [&]() {
            emit_usage();
            StreamEvent done_evt;
            done_evt.type = StreamEventType::Done;
            done_evt.finish_reason = reported_finish_reason;
            callback(done_evt);
            saw_done = true;
        };

        auto write_cb = cpr::WriteCallback{[&](const std::string_view data, intptr_t) -> bool {
            if (abort_flag && abort_flag->load()) {
                return false;
            }

            if (!data.empty()) {
                last_stream_activity_ms.store(steady_now_ms());
            }
            raw_body_capture.append(data.data(), data.size());
            sse_buffer += std::string(data);

            size_t pos = 0;
            size_t delimiter_len = 0;
            while (find_event_delimiter(sse_buffer, pos, delimiter_len)) {
                const std::string event_block = sse_buffer.substr(0, pos);
                sse_buffer.erase(0, pos + delimiter_len);

                SseEventBlock evt_block = parse_sse_event_block(event_block);
                if (evt_block.data.empty()) continue;
                saw_sse_data = true;

                nlohmann::json j;
                try {
                    j = nlohmann::json::parse(evt_block.data);
                } catch (const nlohmann::json::parse_error& e) {
                    saw_parse_error = true;
                    LOG_WARN("Anthropic SSE JSON parse error: " + std::string(e.what()) +
                             " data=" + log_truncate(evt_block.data, 200));
                    continue;
                }

                const std::string event_type = evt_block.event.empty()
                    ? j.value("type", std::string{})
                    : evt_block.event;

                if (event_type == "ping") {
                    continue;
                }
                if (event_type == "error" || j.value("type", std::string{}) == "error") {
                    saw_payload_error = true;
                    payload_error_body = evt_block.data;
                    payload_error_message = anthropic_error_message_from_json(j);
                    LOG_ERROR("Anthropic SSE payload error: " +
                              log_truncate(payload_error_message.empty()
                                  ? evt_block.data
                                  : payload_error_message, 500));
                    return false;
                }
                if (event_type == "message_start") {
                    if (j.contains("message") && j["message"].is_object()) {
                        const auto& message = j["message"];
                        if (message.contains("usage")) {
                            merge_usage(accumulated.usage, message["usage"]);
                        }
                    }
                    continue;
                }
                if (event_type == "content_block_start") {
                    const int index = j.value("index", 0);
                    auto& block = blocks[index];
                    if (j.contains("content_block") && j["content_block"].is_object()) {
                        const auto& cb = j["content_block"];
                        block.type = cb.value("type", std::string{});
                        block.id = cb.value("id", std::string{});
                        block.name = cb.value("name", std::string{});
                        if (cb.contains("input") && !cb["input"].is_null() &&
                            !cb["input"].empty()) {
                            block.input_json = cb["input"].dump();
                        }
                    }
                    continue;
                }
                if (event_type == "content_block_delta") {
                    const int index = j.value("index", 0);
                    if (!j.contains("delta") || !j["delta"].is_object()) continue;
                    const auto& delta = j["delta"];
                    const std::string delta_type = delta.value("type", std::string{});
                    if (delta_type == "text_delta" && delta.contains("text") &&
                        delta["text"].is_string()) {
                        const std::string token = delta["text"].get<std::string>();
                        accumulated.content += token;
                        if (!token.empty()) {
                            emitted_stream_output = true;
                            StreamEvent event;
                            event.type = StreamEventType::Delta;
                            event.content = token;
                            callback(event);
                        }
                    } else if (delta_type == "thinking_delta" &&
                               delta.contains("thinking") &&
                               delta["thinking"].is_string()) {
                        const std::string token = delta["thinking"].get<std::string>();
                        accumulated.reasoning_content += token;
                        if (!token.empty()) {
                            emitted_stream_output = true;
                            StreamEvent event;
                            event.type = StreamEventType::ReasoningDelta;
                            event.content = token;
                            callback(event);
                        }
                    } else if (delta_type == "input_json_delta" &&
                               delta.contains("partial_json") &&
                               delta["partial_json"].is_string()) {
                        auto& block = blocks[index];
                        block.type = "tool_use";
                        block.input_json += delta["partial_json"].get<std::string>();
                        emitted_stream_output = true;

                        StreamEvent progress_evt;
                        progress_evt.type = StreamEventType::ToolCallDelta;
                        progress_evt.tool_index = index;
                        progress_evt.tool_call.id = block.id;
                        progress_evt.tool_call.function_name = block.name;
                        progress_evt.tool_call_argument_bytes = block.input_json.size();
                        callback(progress_evt);
                    }
                    continue;
                }
                if (event_type == "content_block_stop") {
                    const int index = j.value("index", 0);
                    flush_tool_block(index);
                    continue;
                }
                if (event_type == "message_delta") {
                    if (j.contains("delta") && j["delta"].is_object()) {
                        const auto& delta = j["delta"];
                        if (delta.contains("stop_reason") && !delta["stop_reason"].is_null()) {
                            reported_finish_reason = delta["stop_reason"].get<std::string>();
                            accumulated.finish_reason = reported_finish_reason;
                        }
                    }
                    if (j.contains("usage")) {
                        merge_usage(accumulated.usage, j["usage"]);
                    }
                    continue;
                }
                if (event_type == "message_stop") {
                    emit_done();
                    continue;
                }
            }
            return true;
        }};

        cpr::Response r = cpr::Post(
            cpr::Url{url},
            headers,
            cpr::Body{body.dump()},
            cpr::ConnectTimeout{
                (std::min)(stream_idle_timeout_ms, kStreamConnectTimeoutCapMs)},
            network::build_ssl_options(proxy_opts),
            proxy_opts.proxies,
            proxy_opts.auth,
            write_cb,
            progress_cb
        );

        last_accumulated = accumulated;
        const bool user_aborted = abort_flag && abort_flag->load();
        if (user_aborted) {
            auto info = make_provider_error(
                ProviderErrorKind::UserCancelled,
                0,
                name(),
                model_,
                std::string{},
                std::string{},
                std::string{},
                false);
            emit_provider_error(callback, info);
            return accumulated;
        }

        ProviderErrorInfo error_info;
        const std::string request_id = extract_request_id(r.header);
        const bool idle_timeout = stream_idle_timed_out.load();
        const bool transport_failed = static_cast<bool>(r.error);
        const ProviderErrorKind transport_kind =
            idle_timeout ? ProviderErrorKind::Timeout : classify_cpr_error(r.error);
        if (saw_payload_error) {
            error_info = make_provider_error(
                ProviderErrorKind::Unknown,
                0,
                name(),
                model_,
                request_id,
                payload_error_body,
                payload_error_message.empty()
                    ? std::string("stream payload contained an error")
                    : payload_error_message,
                is_retryable_http_status(0, payload_error_body));
        } else if (idle_timeout ||
            (transport_failed && transport_kind == ProviderErrorKind::Timeout)) {
            const int status_code = r.status_code == 0 ? 0 : static_cast<int>(r.status_code);
            const std::string timeout_message = idle_timeout
                ? "stream idle timeout after " +
                    std::to_string(stream_idle_timeout_ms) +
                    " ms without data"
                : (r.error.message.empty()
                    ? std::string("request timed out")
                    : r.error.message);
            error_info = make_provider_error(
                ProviderErrorKind::Timeout,
                status_code,
                name(),
                model_,
                request_id,
                raw_body_capture,
                timeout_message,
                true);
        } else if (r.status_code != 0 && (r.status_code < 200 || r.status_code >= 300)) {
            const std::string err_body = r.text.empty() ? raw_body_capture : r.text;
            error_info = make_provider_error(
                ProviderErrorKind::Http,
                static_cast<int>(r.status_code),
                name(),
                model_,
                request_id,
                err_body,
                std::string{},
                is_retryable_http_status(static_cast<int>(r.status_code), err_body));
        } else if (r.status_code == 0) {
            error_info = make_provider_error(
                transport_kind,
                0,
                name(),
                model_,
                request_id,
                raw_body_capture,
                r.error.message,
                true);
        } else if (!saw_done) {
            const ProviderErrorKind kind = saw_parse_error
                ? ProviderErrorKind::MalformedJson
                : ProviderErrorKind::MalformedSse;
            const std::string transport_message = saw_sse_data
                ? "stream ended before message_stop"
                : "response did not contain SSE data";
            error_info = make_provider_error(
                kind,
                200,
                name(),
                model_,
                request_id,
                raw_body_capture,
                transport_message,
                kind == ProviderErrorKind::MalformedSse && accumulated.tool_calls.empty());
        } else {
            return accumulated;
        }

        const bool can_retry =
            error_info.retryable &&
            (!emitted_stream_output ||
             error_info.kind == ProviderErrorKind::Timeout ||
             error_info.kind == ProviderErrorKind::MalformedSse) &&
            attempt < kStreamMaxAttempts;
        if (can_retry) {
            const int delay_ms = retry_after_delay_ms(r.header, attempt);
            error_info.retry_attempt = attempt;
            error_info.retry_max_attempts = kStreamMaxAttempts - 1;
            error_info.retry_delay_ms = delay_ms;
            emit_retry_event(callback, error_info);
            if (sleep_retry_or_aborted(delay_ms, abort_flag)) {
                auto cancel_info = make_provider_error(
                    ProviderErrorKind::UserCancelled,
                    0,
                    name(),
                    model_,
                    std::string{},
                    std::string{},
                    std::string{},
                    false);
                emit_provider_error(callback, cancel_info);
                return accumulated;
            }
            continue;
        }

        error_info.retry_attempt = (std::max)(0, attempt - 1);
        error_info.retry_max_attempts = kStreamMaxAttempts - 1;
        emit_provider_error(callback, error_info);
        return accumulated;
    }

    return last_accumulated;
}

void AnthropicProvider::chat_stream(
    const std::vector<ChatMessage>& messages,
    const std::vector<ToolDef>& tools,
    const StreamCallback& callback,
    std::atomic<bool>* abort_flag
) {
    if (api_key_.empty()) {
        auto info = make_provider_error(
            ProviderErrorKind::Unknown,
            0,
            name(),
            model_,
            std::string{},
            std::string{},
            "missing Anthropic API key",
            false);
        emit_provider_error(callback, info);
        return;
    }

    nlohmann::json body = build_request_body(messages, tools, true);
    const std::string url = base_url_ + "/messages";

    std::map<std::string, std::string> extra_headers;
    extra_headers["x-api-key"] = api_key_;
    std::string header_error;
    auto resolved_headers = resolve_request_headers(request_headers_, header_error);
    if (!resolved_headers.has_value()) {
        LOG_ERROR("Anthropic request_headers resolution failed: " + header_error);
        StreamEvent evt;
        evt.type = StreamEventType::Error;
        evt.error = header_error;
        callback(evt);
        return;
    }
    for (const auto& [k, v] : *resolved_headers) {
        extra_headers[k] = v;
    }

    parse_sse_stream(url, body, extra_headers, callback, abort_flag);
}

} // namespace acecode
