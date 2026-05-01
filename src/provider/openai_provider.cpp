#include "openai_provider.hpp"
#include "utils/logger.hpp"
#include "network/proxy_resolver.hpp"
#include <cpr/cpr.h>
#include <cpr/ssl_options.h>
#include <stdexcept>
#include <sstream>
#include <map>

namespace acecode {

OpenAiCompatProvider::OpenAiCompatProvider(const std::string& base_url,
                                           const std::string& api_key,
                                           const std::string& model)
    : base_url_(base_url), api_key_(api_key), model_(model) {}

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
    body["messages"] = msgs_json;

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
    ChatResponse accumulated;
    accumulated.finish_reason = "stop";
    std::string sse_buffer;
    // 非 200 响应时 cpr 的 r.text 会是空的(因为 WriteCallback 已消费了 body),
    // 所以并行累积一份原始字节,仅用于错误诊断。16 KB 足够容纳绝大多数 JSON 报错。
    std::string raw_body_capture;
    constexpr size_t kRawBodyCaptureLimit = 16 * 1024;

    LOG_INFO("parse_sse_stream url=" + url);
    LOG_DEBUG("Request body: " + log_truncate(body.dump(), 500));

    // Track in-progress tool calls by index
    struct ToolCallAccumulator {
        std::string id;
        std::string name;
        std::string arguments;
    };
    std::map<int, ToolCallAccumulator> pending_tools;

    // Build cpr headers
    cpr::Header headers = {{"Content-Type", "application/json"}};
    for (const auto& [k, v] : extra_headers) {
        headers[k] = v;
    }

    auto write_cb = cpr::WriteCallback{[&](const std::string_view data, intptr_t) -> bool {
        if (abort_flag && abort_flag->load()) {
            return false; // cancel request
        }

        if (raw_body_capture.size() < kRawBodyCaptureLimit) {
            size_t remain = kRawBodyCaptureLimit - raw_body_capture.size();
            size_t take = (remain < data.size()) ? remain : data.size();
            raw_body_capture.append(data.data(), take);
        }

        sse_buffer += std::string(data);

        // Process complete SSE events (separated by \n\n)
        size_t pos;
        while ((pos = sse_buffer.find("\n\n")) != std::string::npos) {
            std::string event_block = sse_buffer.substr(0, pos);
            sse_buffer.erase(0, pos + 2);

            // Extract data lines
            std::istringstream iss(event_block);
            std::string line;
            std::string event_data;
            while (std::getline(iss, line)) {
                // Remove trailing \r if present
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                // SSE 规范(HTML5 EventSource)允许 "data:" 后的单个前导空格可选 ——
                // OpenAI / DeepSeek 等主流服务发 "data: {...}",但部分自建网关
                // 发 "data:{...}" 不带空格,
                // 必须两者都接受,否则 chunk 全被静默丢弃,表现为"消息发出去无返回"。
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
            if (event_data == "[DONE]") {
                LOG_DEBUG("SSE [DONE] received. pending_tools=" + std::to_string(pending_tools.size()));
                // Flush any remaining tool calls
                for (auto& [idx, tc] : pending_tools) {
                    ToolCall call;
                    call.id = tc.id;
                    call.function_name = tc.name;
                    call.function_arguments = tc.arguments;
                    accumulated.tool_calls.push_back(call);

                    StreamEvent evt;
                    evt.type = StreamEventType::ToolCall;
                    evt.tool_call = call;
                    callback(evt);
                }
                pending_tools.clear();

                // Emit usage event if we collected any
                if (accumulated.usage.has_data) {
                    StreamEvent usage_evt;
                    usage_evt.type = StreamEventType::Usage;
                    usage_evt.usage = accumulated.usage;
                    callback(usage_evt);
                }

                StreamEvent done_evt;
                done_evt.type = StreamEventType::Done;
                callback(done_evt);
                return true;
            }

            try {
                auto j = nlohmann::json::parse(event_data);

                // Parse usage from SSE chunk (typically in the final chunk)
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

                // Reasoning chain-of-thought delta. DeepSeek primary name is
                // `reasoning_content`; OpenRouter/Qwen alias is `reasoning`.
                // If both are present in the same chunk we prefer
                // reasoning_content (consistent with non-streaming parse).
                std::string reasoning_token;
                if (delta.contains("reasoning_content") && delta["reasoning_content"].is_string()) {
                    reasoning_token = delta["reasoning_content"].get<std::string>();
                } else if (delta.contains("reasoning") && delta["reasoning"].is_string()) {
                    reasoning_token = delta["reasoning"].get<std::string>();
                }
                if (!reasoning_token.empty()) {
                    accumulated.reasoning_content += reasoning_token;

                    StreamEvent evt;
                    evt.type = StreamEventType::ReasoningDelta;
                    evt.content = reasoning_token;
                    callback(evt);
                }

                // Text content delta
                if (delta.contains("content") && !delta["content"].is_null()) {
                    std::string token = delta["content"].get<std::string>();
                    accumulated.content += token;

                    StreamEvent evt;
                    evt.type = StreamEventType::Delta;
                    evt.content = token;
                    callback(evt);
                }

                // Tool calls delta
                if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
                    for (const auto& tc_delta : delta["tool_calls"]) {
                        int index = tc_delta.value("index", 0);
                        auto& acc = pending_tools[index];

                        const bool has_id = tc_delta.contains("id") && !tc_delta["id"].is_null();
                        std::string incoming_id = has_id ? tc_delta["id"].get<std::string>() : std::string();

                        // 部分非标 OpenAI 兼容服务(实测 GLM-5.1-FP8 走某些公司 wizard-ai 网关)
                        // 在每一帧 SSE 里都重发 *相同的* tool_call id 和 *完整的*
                        // arguments JSON(全量快照,不是增量)。OpenAI 标准是首帧带
                        // id+name、后续帧仅带 arguments 增量。若一律 append,GLM 流会被
                        // 拼成 `{...}{...}` 两份 JSON 头尾相接,parse 必失败,UI 显示
                        // "Failed to parse tool arguments"。
                        // 检测:同一 index 上 incoming id 与已记录 id 相同 → 这条 chunk
                        // *可能是* 全量帧,但还需校验 incoming_args 是 acc.arguments 的
                        // 前缀超集才用替换语义 —— 否则 (a) 空确认帧会擦掉已累积的参数
                        // (实测 monotoo.shop 网关上 web_search/bash 全部失败:
                        // "Web search failed: invalid JSON arguments: ... empty input"),
                        // (b) 某些网关重发 id 但 arguments 仍是 delta 片段,替换会只剩
                        // 最后一帧的尾段。
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
                                    // 空确认帧:无论 is_full_snapshot_resend 真假,都不要
                                    // 用 "" 覆盖或追加 —— 这是导致 monotoo.shop 上空 args
                                    // bug 的直接原因。保持已累积的值不变。
                                } else if (is_full_snapshot_resend &&
                                           incoming_args.size() >= acc.arguments.size() &&
                                           incoming_args.compare(0, acc.arguments.size(),
                                                                 acc.arguments) == 0) {
                                    // 真正的全量快照:incoming 必然包含已累积的所有内容
                                    // 作为前缀。GLM 风格逐帧增长的快照流满足此条件,
                                    // 此时用替换语义合并,避免 `{...}{...}` 头尾相接。
                                    acc.arguments = incoming_args;
                                } else {
                                    // 标准 OpenAI 增量(无 id 的后续帧),或者带 id 但
                                    // arguments 不是前缀超集的"伪快照" delta 流 —— 一律
                                    // 追加,把碎片还原成完整 JSON。
                                    acc.arguments += incoming_args;
                                }
                            }
                        }
                    }
                }

                // If finish_reason is tool_calls or stop, flush tool calls
                if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
                    for (auto& [idx, tc] : pending_tools) {
                        ToolCall call;
                        call.id = tc.id;
                        call.function_name = tc.name;
                        call.function_arguments = tc.arguments;
                        accumulated.tool_calls.push_back(call);

                        StreamEvent evt;
                        evt.type = StreamEventType::ToolCall;
                        evt.tool_call = call;
                        callback(evt);
                    }
                    pending_tools.clear();
                }
            } catch (const nlohmann::json::parse_error& e) {
                LOG_WARN("SSE JSON parse error: " + std::string(e.what()) + " data=" + log_truncate(event_data, 200));
                // Skip malformed JSON chunks
            }
        }
        return true;
    }};

    // libcurl invokes this ~1 Hz regardless of whether bytes are flowing, so it
    // is the only path that can cancel during a model's silent reasoning phase.
    auto progress_cb = cpr::ProgressCallback{
        [abort_flag](cpr::cpr_pf_arg_t /*dl_total*/, cpr::cpr_pf_arg_t /*dl_now*/,
                     cpr::cpr_pf_arg_t /*ul_total*/, cpr::cpr_pf_arg_t /*ul_now*/,
                     intptr_t /*userdata*/) -> bool {
            return !(abort_flag && abort_flag->load());
        }
    };

    auto proxy_opts = network::proxy_options_for(url);
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

    const bool aborted_by_callback = (r.error.code == cpr::ErrorCode::ABORTED_BY_CALLBACK);
    if ((abort_flag && abort_flag->load()) || aborted_by_callback) {
        if (aborted_by_callback && accumulated.content.empty() && pending_tools.empty()) {
            LOG_WARN("SSE request aborted by user (no-data phase or progress callback)");
        } else {
            LOG_WARN("SSE request aborted by user");
        }
        StreamEvent evt;
        evt.type = StreamEventType::Error;
        evt.error = "Request cancelled";
        callback(evt);
        return accumulated;
    }

    if (r.status_code != 0 && r.status_code != 200) {
        const std::string& err_body = r.text.empty() ? raw_body_capture : r.text;
        LOG_ERROR("SSE HTTP error: " + std::to_string(r.status_code) + " body=" + log_truncate(err_body, 2000));
        StreamEvent evt;
        evt.type = StreamEventType::Error;
        evt.error = "HTTP " + std::to_string(r.status_code) + ": " + err_body;
        callback(evt);
    }

    if (r.status_code == 0) {
        LOG_ERROR("SSE connection failed: " + r.error.message);
        StreamEvent evt;
        evt.type = StreamEventType::Error;
        evt.error = "Connection failed: " + r.error.message;
        callback(evt);
    }

    return accumulated;
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
