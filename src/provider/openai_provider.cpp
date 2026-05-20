#include "openai_provider.hpp"
#include "utils/logger.hpp"
#include "network/proxy_resolver.hpp"
#include <cpr/cpr.h>
#include <cpr/ssl_options.h>
#include <stdexcept>
#include <sstream>
#include <map>
#include <unordered_set>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <thread>

namespace acecode {

OpenAiCompatProvider::OpenAiCompatProvider(const std::string& base_url,
                                           const std::string& api_key,
                                           const std::string& model)
    : base_url_(base_url), api_key_(api_key), model_(model) {}

namespace {

constexpr int kStreamMaxAttempts = 3;
constexpr int kStreamRetryBaseDelayMs = 100;
constexpr int kStreamRetryMaxDelayMs = 2000;
constexpr int kStreamRetrySleepSliceMs = 50;

std::string ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string provider_error_kind_to_string(ProviderErrorKind kind) {
    switch (kind) {
    case ProviderErrorKind::None:         return "none";
    case ProviderErrorKind::UserCancelled:return "user_cancelled";
    case ProviderErrorKind::Timeout:      return "timeout";
    case ProviderErrorKind::Network:      return "network";
    case ProviderErrorKind::Http:         return "http";
    case ProviderErrorKind::MalformedSse: return "malformed_sse";
    case ProviderErrorKind::MalformedJson:return "malformed_json";
    case ProviderErrorKind::Unknown:      return "unknown";
    }
    return "unknown";
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

std::string header_value_ci(const cpr::Header& headers, const std::string& key) {
    const std::string wanted = ascii_lower(key);
    for (const auto& [header_key, value] : headers) {
        if (ascii_lower(header_key) == wanted) return value;
    }
    return {};
}

std::string extract_request_id(const cpr::Header& headers) {
    for (const std::string& key : {
             "x-request-id",
             "request-id",
             "x-github-request-id",
             "x-ms-request-id",
             "cf-ray",
         }) {
        std::string value = header_value_ci(headers, key);
        if (!value.empty()) return value;
    }
    return {};
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

std::string provider_error_prefix(ProviderErrorKind kind, int status_code) {
    switch (kind) {
    case ProviderErrorKind::UserCancelled:
        return "Request cancelled";
    case ProviderErrorKind::Timeout:
        return "Request timed out";
    case ProviderErrorKind::Network:
        return "Connection failed";
    case ProviderErrorKind::Http:
        return "HTTP " + std::to_string(status_code);
    case ProviderErrorKind::MalformedJson:
        return "Malformed JSON in streaming response";
    case ProviderErrorKind::MalformedSse:
        return "Malformed or incomplete streaming response";
    case ProviderErrorKind::Unknown:
        return "Provider request failed";
    case ProviderErrorKind::None:
        return {};
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
    for (int i = 1; i < attempt_index; ++i) {
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

} // namespace

nlohmann::json OpenAiCompatProvider::build_request_body(
    const std::vector<ChatMessage>& messages,
    const std::vector<ToolDef>& tools,
    bool stream
) const {
    nlohmann::json body;
    body["model"] = model_;
    if (stream) {
        body["stream"] = true;
        body["stream_options"] = {{"include_usage", true}};
    }

    // Build messages array.
    // 最后一道防御:OpenAI/Ark 严格要求 role ∈ {system,user,assistant,tool}。
    // 任何非法 role(例如历史遗留的 UI-only `tool_result`)在此直接丢弃并 warn,
    // 防止 resume/压缩/旧 session 等路径意外污染 messages_ 时把整个请求打挂。
    nlohmann::json msgs_json = nlohmann::json::array();
    int dropped_invalid_role = 0;
    for (const auto& msg : messages) {
        const bool valid_role = (msg.role == "system" || msg.role == "user" ||
                                 msg.role == "assistant" || msg.role == "tool");
        if (!valid_role) {
            ++dropped_invalid_role;
            LOG_WARN("build_request_body: dropping message with invalid role='" +
                     msg.role + "' content=" + log_truncate(msg.content, 200));
            continue;
        }

        nlohmann::json m;
        m["role"] = msg.role;

        if (msg.role == "assistant" && !msg.tool_calls.is_null() && !msg.tool_calls.empty()) {
            // Assistant message with tool calls
            if (!msg.content.empty()) {
                m["content"] = msg.content;
            } else {
                m["content"] = nullptr;
            }
            m["tool_calls"] = msg.tool_calls;
        } else if (msg.role == "tool") {
            m["content"] = msg.content;
            m["tool_call_id"] = msg.tool_call_id;
        } else {
            m["content"] = msg.content;
        }

        // Echo reasoning_content back on assistant messages only. DeepSeek
        // thinking-mode rejects the next request with HTTP 400 if the previous
        // turn produced reasoning_content and we don't include it here.
        // Other OpenAI-compatible servers ignore unknown fields.
        if (msg.role == "assistant" && !msg.reasoning_content.empty()) {
            m["reasoning_content"] = msg.reasoning_content;
        }

        msgs_json.push_back(m);
    }
    if (dropped_invalid_role > 0) {
        LOG_WARN("build_request_body: total " + std::to_string(dropped_invalid_role) +
                 " message(s) dropped due to invalid role");
    }

    // Orphan tool_call 防御:OpenAI / Chat Completions 严格要求
    // assistant.tool_calls[*].id 都必须有一条 role=tool, tool_call_id=<id>
    // 紧跟在后面;否则整个请求 400 "No tool output found for function call …"。
    // 历史 session 在以下场景会持久化出 orphan(被打断的工具调用):
    //   - daemon 中途被 kill / 崩溃,assistant 已写盘但 tool result 未来得及落
    //   - 工具执行抛异常路径里 ToolResult 没被 append
    //   - resume 选了一个旧 jsonl,正好截在 assistant 之后
    // 一旦有 orphan,该 session 的下一条 user 消息会让 LLM 端永久作废。我们在
    // 请求出口把 orphan 用占位 tool 消息补齐,让 session 自动复活;同时 WARN
    // 一条供排查。msgs_ 内存与 session jsonl 都不动 — 只在序列化层补洞。
    nlohmann::json patched = nlohmann::json::array();
    int synthesized_stubs = 0;
    {
        size_t n = msgs_json.size();
        for (size_t i = 0; i < n; ) {
            const auto& cur = msgs_json[i];
            patched.push_back(cur);
            const std::string role = cur.value("role", std::string{});
            if (role == "assistant" && cur.contains("tool_calls") &&
                cur["tool_calls"].is_array() && !cur["tool_calls"].empty()) {
                std::vector<std::string> needed_ids;
                for (const auto& tc : cur["tool_calls"]) {
                    if (tc.contains("id") && tc["id"].is_string()) {
                        needed_ids.push_back(tc["id"].get<std::string>());
                    }
                }
                std::unordered_set<std::string> seen_ids;
                size_t j = i + 1;
                while (j < n && msgs_json[j].value("role", std::string{}) == "tool") {
                    patched.push_back(msgs_json[j]);
                    if (msgs_json[j].contains("tool_call_id") &&
                        msgs_json[j]["tool_call_id"].is_string()) {
                        seen_ids.insert(msgs_json[j]["tool_call_id"].get<std::string>());
                    }
                    ++j;
                }
                for (const auto& id : needed_ids) {
                    if (!seen_ids.count(id)) {
                        nlohmann::json stub;
                        stub["role"] = "tool";
                        stub["tool_call_id"] = id;
                        stub["content"] =
                            "[Error] Tool execution was interrupted before it could "
                            "produce a result. No output is available for this call.";
                        patched.push_back(std::move(stub));
                        ++synthesized_stubs;
                    }
                }
                i = j;
            } else {
                ++i;
            }
        }
    }
    if (synthesized_stubs > 0) {
        LOG_WARN("build_request_body: synthesized " + std::to_string(synthesized_stubs) +
                 " placeholder tool message(s) for orphan tool_call(s) "
                 "(likely from interrupted prior turns; session will continue)");
    }
    body["messages"] = patched;

    // Build tools array
    if (!tools.empty()) {
        nlohmann::json tools_json = nlohmann::json::array();
        for (const auto& tool : tools) {
            nlohmann::json t;
            t["type"] = "function";
            t["function"]["name"] = tool.name;
            t["function"]["description"] = tool.description;
            t["function"]["parameters"] = tool.parameters;
            tools_json.push_back(t);
        }
        body["tools"] = tools_json;
    }

    return body;
}

ChatResponse OpenAiCompatProvider::parse_response(const nlohmann::json& j) {
    ChatResponse resp;

    if (!j.contains("choices") || j["choices"].empty()) {
        resp.content = "[Error] No choices in API response.";
        resp.finish_reason = "error";
        return resp;
    }

    const auto& choice = j["choices"][0];
    resp.finish_reason = choice.value("finish_reason", "stop");

    const auto& message = choice["message"];

    if (message.contains("content") && !message["content"].is_null()) {
        resp.content = message["content"].get<std::string>();
    }

    // Reasoning-mode chain-of-thought. DeepSeek primary name is
    // `reasoning_content`; OpenRouter/Qwen alias is `reasoning`.
    if (message.contains("reasoning_content") && message["reasoning_content"].is_string()) {
        resp.reasoning_content = message["reasoning_content"].get<std::string>();
    } else if (message.contains("reasoning") && message["reasoning"].is_string()) {
        resp.reasoning_content = message["reasoning"].get<std::string>();
    }

    if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
        for (const auto& tc : message["tool_calls"]) {
            ToolCall call;
            call.id = tc["id"].get<std::string>();
            call.function_name = tc["function"]["name"].get<std::string>();
            call.function_arguments = tc["function"]["arguments"].get<std::string>();
            resp.tool_calls.push_back(call);
        }
    }

    return resp;
}

ChatResponse OpenAiCompatProvider::chat(
    const std::vector<ChatMessage>& messages,
    const std::vector<ToolDef>& tools
) {
    nlohmann::json body = build_request_body(messages, tools, false);

    std::string url = base_url_ + "/chat/completions";

    cpr::Header headers = {
        {"Content-Type", "application/json"}
    };
    if (!api_key_.empty()) {
        headers["Authorization"] = "Bearer " + api_key_;
    }

    auto proxy_opts = network::proxy_options_for(url);
    cpr::Response r = cpr::Post(
        cpr::Url{url},
        headers,
        cpr::Body{body.dump()},
        network::build_ssl_options(proxy_opts),
        proxy_opts.proxies,
        proxy_opts.auth,
        cpr::Timeout{120000} // 2 minutes timeout for LLM responses
    );

    if (r.status_code == 0) {
        ChatResponse resp;
        resp.content = "[Error] Connection failed: " + r.error.message;
        resp.finish_reason = "error";
        return resp;
    }

    if (r.status_code != 200) {
        ChatResponse resp;
        resp.content = "[Error] HTTP " + std::to_string(r.status_code) + ": " + r.text;
        resp.finish_reason = "error";
        return resp;
    }

    try {
        nlohmann::json response_json = nlohmann::json::parse(r.text);
        auto resp = parse_response(response_json);
        // Parse usage from non-streaming response
        if (response_json.contains("usage") && response_json["usage"].is_object()) {
            const auto& u = response_json["usage"];
            resp.usage.prompt_tokens = u.value("prompt_tokens", 0);
            resp.usage.completion_tokens = u.value("completion_tokens", 0);
            resp.usage.total_tokens = u.value("total_tokens", 0);
            if (u.contains("prompt_tokens_details") && u["prompt_tokens_details"].is_object()) {
                const auto& d = u["prompt_tokens_details"];
                resp.usage.cache_read_tokens = d.value("cached_tokens", 0);
                resp.usage.cache_write_tokens = d.value("cache_write_tokens", 0);
            }
            if (u.contains("completion_tokens_details") && u["completion_tokens_details"].is_object()) {
                const auto& d = u["completion_tokens_details"];
                resp.usage.reasoning_tokens = d.value("reasoning_tokens", 0);
            }
            resp.usage.has_data = true;
        }
        return resp;
    } catch (const nlohmann::json::parse_error& e) {
        ChatResponse resp;
        resp.content = "[Error] Failed to parse response JSON: " + std::string(e.what());
        resp.finish_reason = "error";
        return resp;
    }
}

// ---- SSE stream parsing ----

ChatResponse OpenAiCompatProvider::parse_sse_stream(
    const std::string& url,
    const nlohmann::json& body,
    const std::map<std::string, std::string>& extra_headers,
    const StreamCallback& callback,
    std::atomic<bool>* abort_flag
) {
    LOG_INFO("parse_sse_stream url=" + url);
    LOG_DEBUG("Request body: " + log_truncate(body.dump(), 500));

    // Build cpr headers
    cpr::Header headers = {{"Content-Type", "application/json"}};
    for (const auto& [k, v] : extra_headers) {
        headers[k] = v;
    }

    // libcurl invokes this ~1 Hz regardless of whether bytes are flowing, so it
    // is the only path that can cancel during a model's silent reasoning phase.
    auto progress_cb = cpr::ProgressCallback{
        [abort_flag](cpr::cpr_off_t /*dl_total*/, cpr::cpr_off_t /*dl_now*/,
                     cpr::cpr_off_t /*ul_total*/, cpr::cpr_off_t /*ul_now*/,
                     intptr_t /*userdata*/) -> bool {
            return !(abort_flag && abort_flag->load());
        }
    };

    auto proxy_opts = network::proxy_options_for(url);

    struct ToolCallAccumulator {
        std::string id;
        std::string name;
        std::string arguments;
    };

    ChatResponse last_accumulated;
    last_accumulated.finish_reason = "stop";

    for (int attempt = 1; attempt <= kStreamMaxAttempts; ++attempt) {
        ChatResponse accumulated;
        accumulated.finish_reason = "stop";
        std::string sse_buffer;
        std::string raw_body_capture;
        std::map<int, ToolCallAccumulator> pending_tools;
        bool saw_done = false;
        bool saw_sse_data = false;
        bool saw_parse_error = false;
        bool emitted_stream_output = false;

        auto flush_pending_tools = [&]() {
            for (auto& [idx, tc] : pending_tools) {
                ToolCall call;
                call.id = tc.id;
                call.function_name = tc.name;
                call.function_arguments = tc.arguments;
                accumulated.tool_calls.push_back(call);

                StreamEvent evt;
                evt.type = StreamEventType::ToolCall;
                evt.tool_call = call;
                evt.tool_index = idx;
                callback(evt);
            }
            pending_tools.clear();
        };

        auto emit_done = [&]() {
            flush_pending_tools();
            if (accumulated.usage.has_data) {
                StreamEvent usage_evt;
                usage_evt.type = StreamEventType::Usage;
                usage_evt.usage = accumulated.usage;
                callback(usage_evt);
            }

            StreamEvent done_evt;
            done_evt.type = StreamEventType::Done;
            callback(done_evt);
            saw_done = true;
        };

        auto find_event_delimiter = [](const std::string& buffer,
                                       size_t& pos,
                                       size_t& delimiter_len) -> bool {
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
        };

        auto write_cb = cpr::WriteCallback{[&](const std::string_view data, intptr_t) -> bool {
            if (abort_flag && abort_flag->load()) {
                return false; // cancel request
            }

            raw_body_capture.append(data.data(), data.size());
            sse_buffer += std::string(data);

            size_t pos = 0;
            size_t delimiter_len = 0;
            while (find_event_delimiter(sse_buffer, pos, delimiter_len)) {
                std::string event_block = sse_buffer.substr(0, pos);
                sse_buffer.erase(0, pos + delimiter_len);

                std::istringstream iss(event_block);
                std::string line;
                std::string event_data;
                while (std::getline(iss, line)) {
                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }
                    static constexpr std::string_view kDataPrefix = "data:";
                    if (line.compare(0, kDataPrefix.size(), kDataPrefix) == 0) {
                        size_t value_start = kDataPrefix.size();
                        if (value_start < line.size() && line[value_start] == ' ') {
                            ++value_start;
                        }
                        if (!event_data.empty()) event_data += "\n";
                        event_data += line.substr(value_start);
                    }
                }

                if (event_data.empty()) continue;
                saw_sse_data = true;
                if (event_data == "[DONE]") {
                    LOG_DEBUG("SSE [DONE] received. pending_tools=" + std::to_string(pending_tools.size()));
                    emit_done();
                    continue;
                }

                try {
                    auto j = nlohmann::json::parse(event_data);

                    if (j.contains("usage") && j["usage"].is_object()) {
                        const auto& u = j["usage"];
                        accumulated.usage.prompt_tokens = u.value("prompt_tokens", 0);
                        accumulated.usage.completion_tokens = u.value("completion_tokens", 0);
                        accumulated.usage.total_tokens = u.value("total_tokens", 0);
                        if (u.contains("prompt_tokens_details") && u["prompt_tokens_details"].is_object()) {
                            const auto& d = u["prompt_tokens_details"];
                            accumulated.usage.cache_read_tokens = d.value("cached_tokens", 0);
                            accumulated.usage.cache_write_tokens = d.value("cache_write_tokens", 0);
                        }
                        if (u.contains("completion_tokens_details") && u["completion_tokens_details"].is_object()) {
                            const auto& d = u["completion_tokens_details"];
                            accumulated.usage.reasoning_tokens = d.value("reasoning_tokens", 0);
                        }
                        accumulated.usage.has_data = true;
                    }

                    if (!j.contains("choices") || j["choices"].empty()) continue;

                    const auto& choice = j["choices"][0];
                    if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
                        accumulated.finish_reason = choice["finish_reason"].get<std::string>();
                    }

                    if (!choice.contains("delta")) continue;
                    const auto& delta = choice["delta"];

                    std::string reasoning_token;
                    if (delta.contains("reasoning_content") && delta["reasoning_content"].is_string()) {
                        reasoning_token = delta["reasoning_content"].get<std::string>();
                    } else if (delta.contains("reasoning") && delta["reasoning"].is_string()) {
                        reasoning_token = delta["reasoning"].get<std::string>();
                    }
                    if (!reasoning_token.empty()) {
                        accumulated.reasoning_content += reasoning_token;
                        emitted_stream_output = true;

                        StreamEvent evt;
                        evt.type = StreamEventType::ReasoningDelta;
                        evt.content = reasoning_token;
                        callback(evt);
                    }

                    if (delta.contains("content") && !delta["content"].is_null()) {
                        std::string token = delta["content"].get<std::string>();
                        accumulated.content += token;
                        if (!token.empty()) emitted_stream_output = true;

                        StreamEvent evt;
                        evt.type = StreamEventType::Delta;
                        evt.content = token;
                        callback(evt);
                    }

                    if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
                        for (const auto& tc_delta : delta["tool_calls"]) {
                            int index = tc_delta.value("index", 0);
                            auto& acc = pending_tools[index];
                            const std::string before_id = acc.id;
                            const std::string before_name = acc.name;
                            const std::size_t before_args_size = acc.arguments.size();

                            const bool has_id = tc_delta.contains("id") && !tc_delta["id"].is_null();
                            std::string incoming_id = has_id ? tc_delta["id"].get<std::string>() : std::string();
                            const bool is_full_snapshot_resend =
                                has_id && !acc.id.empty() && acc.id == incoming_id;

                            if (has_id) {
                                acc.id = incoming_id;
                            }
                            if (tc_delta.contains("function")) {
                                const auto& fn = tc_delta["function"];
                                if (fn.contains("name") && !fn["name"].is_null()) {
                                    acc.name = fn["name"].get<std::string>();
                                }
                                if (fn.contains("arguments") && !fn["arguments"].is_null()) {
                                    std::string incoming_args = fn["arguments"].get<std::string>();
                                    if (incoming_args.empty()) {
                                        // Keep already accumulated args; some gateways send empty confirmation frames.
                                    } else if (is_full_snapshot_resend &&
                                               incoming_args.size() >= acc.arguments.size() &&
                                               incoming_args.compare(0, acc.arguments.size(),
                                                                     acc.arguments) == 0) {
                                        acc.arguments = incoming_args;
                                    } else {
                                        acc.arguments += incoming_args;
                                    }
                                }
                            }
                            if (acc.id != before_id || acc.name != before_name ||
                                acc.arguments.size() != before_args_size) {
                                emitted_stream_output = true;
                                StreamEvent progress_evt;
                                progress_evt.type = StreamEventType::ToolCallDelta;
                                progress_evt.tool_index = index;
                                progress_evt.tool_call.id = acc.id;
                                progress_evt.tool_call.function_name = acc.name;
                                progress_evt.tool_call_argument_bytes = acc.arguments.size();
                                callback(progress_evt);
                            }
                        }
                    }

                    if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
                        flush_pending_tools();
                        if (!accumulated.tool_calls.empty()) emitted_stream_output = true;
                    }
                } catch (const nlohmann::json::parse_error& e) {
                    saw_parse_error = true;
                    LOG_WARN("SSE JSON parse error: " + std::string(e.what()) +
                             " data=" + log_truncate(event_data, 200));
                }
            }
            return true;
        }};

        cpr::Response r = cpr::Post(
            cpr::Url{url},
            headers,
            cpr::Body{body.dump()},
            network::build_ssl_options(proxy_opts),
            proxy_opts.proxies,
            proxy_opts.auth,
            cpr::Timeout{180000},
            write_cb,
            progress_cb
        );

        last_accumulated = accumulated;
        const bool user_aborted = abort_flag && abort_flag->load();
        if (user_aborted) {
            if (accumulated.content.empty() && pending_tools.empty()) {
                LOG_WARN("SSE request aborted by user (no-data phase or progress callback)");
            } else {
                LOG_WARN("SSE request aborted by user");
            }
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
        if (r.status_code != 0 && r.status_code != 200) {
            const std::string err_body = r.text.empty() ? raw_body_capture : r.text;
            LOG_ERROR("SSE HTTP error: " + std::to_string(r.status_code) +
                      " body=" + log_truncate(err_body, 2000));
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
            const ProviderErrorKind kind = classify_cpr_error(r.error);
            LOG_ERROR("SSE connection failed: " + r.error.message);
            error_info = make_provider_error(
                kind,
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
                ? "stream ended before [DONE]"
                : "response did not contain SSE data";
            LOG_ERROR("SSE malformed response: " + transport_message +
                      " body=" + log_truncate(raw_body_capture, 2000));
            error_info = make_provider_error(
                kind,
                200,
                name(),
                model_,
                request_id,
                raw_body_capture,
                transport_message,
                false);
        } else {
            return accumulated;
        }

        const bool can_retry =
            error_info.retryable &&
            !emitted_stream_output &&
            attempt < kStreamMaxAttempts;
        if (can_retry) {
            const int delay_ms = retry_after_delay_ms(r.header, attempt);
            error_info.retry_attempt = attempt;
            error_info.retry_max_attempts = kStreamMaxAttempts - 1;
            error_info.retry_delay_ms = delay_ms;
            LOG_WARN("Retrying streaming request after " +
                     provider_error_kind_to_string(error_info.kind) +
                     " failure, attempt " + std::to_string(attempt + 1) +
                     "/" + std::to_string(kStreamMaxAttempts));
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

void OpenAiCompatProvider::chat_stream(
    const std::vector<ChatMessage>& messages,
    const std::vector<ToolDef>& tools,
    const StreamCallback& callback,
    std::atomic<bool>* abort_flag
) {
    nlohmann::json body = build_request_body(messages, tools, true);
    std::string url = base_url_ + "/chat/completions";

    std::map<std::string, std::string> extra_headers;
    if (!api_key_.empty()) {
        extra_headers["Authorization"] = "Bearer " + api_key_;
    }

    parse_sse_stream(url, body, extra_headers, callback, abort_flag);
}

} // namespace acecode
