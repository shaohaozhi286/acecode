#include "openai_provider.hpp"
#include "utils/logger.hpp"
#include <cpr/cpr.h>
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
    }

    // Build messages array
    nlohmann::json msgs_json = nlohmann::json::array();
    for (const auto& msg : messages) {
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

        msgs_json.push_back(m);
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

    cpr::Response r = cpr::Post(
        cpr::Url{url},
        headers,
        cpr::Body{body.dump()},
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
        return parse_response(response_json);
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

    auto write_cb = cpr::WriteCallback{[&](const std::string& data, intptr_t) -> bool {
        if (abort_flag && abort_flag->load()) {
            return false; // cancel request
        }

        sse_buffer += data;

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
                if (line.rfind("data: ", 0) == 0) {
                    if (!event_data.empty()) event_data += "\n";
                    event_data += line.substr(6);
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

                StreamEvent done_evt;
                done_evt.type = StreamEventType::Done;
                callback(done_evt);
                return true;
            }

            try {
                auto j = nlohmann::json::parse(event_data);
                if (!j.contains("choices") || j["choices"].empty()) continue;

                const auto& choice = j["choices"][0];
                if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
                    accumulated.finish_reason = choice["finish_reason"].get<std::string>();
                }

                if (!choice.contains("delta")) continue;
                const auto& delta = choice["delta"];

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

                        if (tc_delta.contains("id") && !tc_delta["id"].is_null()) {
                            acc.id = tc_delta["id"].get<std::string>();
                        }
                        if (tc_delta.contains("function")) {
                            const auto& fn = tc_delta["function"];
                            if (fn.contains("name") && !fn["name"].is_null()) {
                                acc.name = fn["name"].get<std::string>();
                            }
                            if (fn.contains("arguments") && !fn["arguments"].is_null()) {
                                acc.arguments += fn["arguments"].get<std::string>();
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

    cpr::Response r = cpr::Post(
        cpr::Url{url},
        headers,
        cpr::Body{body.dump()},
        cpr::Timeout{180000},
        write_cb
    );

    if (abort_flag && abort_flag->load()) {
        LOG_WARN("SSE request aborted by user");
        StreamEvent evt;
        evt.type = StreamEventType::Error;
        evt.error = "Request cancelled";
        callback(evt);
        return accumulated;
    }

    if (r.status_code != 0 && r.status_code != 200) {
        LOG_ERROR("SSE HTTP error: " + std::to_string(r.status_code) + " body=" + log_truncate(r.text, 500));
        StreamEvent evt;
        evt.type = StreamEventType::Error;
        evt.error = "HTTP " + std::to_string(r.status_code) + ": " + r.text;
        callback(evt);
    }

    if (r.status_code == 0 && !(abort_flag && abort_flag->load())) {
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
