#include "codex_provider.hpp"

#include "codex/codex_app_server_client.hpp"
#include "../utils/logger.hpp"
#include "../utils/utf8_path.hpp"

#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <sstream>

namespace acecode {
namespace {

std::string current_cwd_utf8() {
    std::error_code ec;
    auto cwd = std::filesystem::current_path(ec);
    if (ec) return {};
    return path_to_utf8(cwd);
}

std::string role_label(const ChatMessage& message) {
    if (message.role == "system") return "System";
    if (message.role == "assistant") return "Assistant";
    if (message.role == "tool") return "Tool";
    if (message.role == "user") return "User";
    return "Message";
}

std::string build_codex_input_text(const std::vector<ChatMessage>& messages) {
    std::ostringstream out;
    out << "Continue this ACECode conversation. Preserve the user's latest "
           "request as the active task.\n\n";
    for (const auto& message : messages) {
        if (message.is_meta || message.content.empty()) continue;
        out << "### " << role_label(message);
        if (!message.tool_call_id.empty()) out << " tool_call_id=" << message.tool_call_id;
        out << "\n" << message.content << "\n\n";
        if (message.role == "assistant" && !message.tool_calls.is_null() &&
            !message.tool_calls.empty()) {
            out << "Assistant tool calls:\n" << message.tool_calls.dump() << "\n\n";
        }
    }
    return out.str();
}

void emit_error(const StreamCallback& callback,
                const std::string& model,
                const std::string& message) {
    ProviderErrorInfo info;
    info.kind = ProviderErrorKind::Unknown;
    info.provider = "codex";
    info.model = model;
    info.display_message = message;

    StreamEvent evt;
    evt.type = StreamEventType::Error;
    evt.error = message;
    evt.provider_error = std::move(info);
    callback(evt);
}

std::string turn_error_message(const nlohmann::json& turn) {
    if (!turn.is_object()) return "Codex turn failed";
    if (turn.contains("error") && turn["error"].is_object()) {
        const auto& err = turn["error"];
        if (err.contains("message") && err["message"].is_string()) {
            return err["message"].get<std::string>();
        }
    }
    return "Codex turn status: " + turn.value("status", std::string{"unknown"});
}

} // namespace

CodexProvider::CodexProvider(std::string model)
    : model_(std::move(model)) {}

bool CodexProvider::is_authenticated() {
    codex::AppServerClient client;
    std::string error;
    if (!client.start(&error) || !client.initialize(&error)) {
        LOG_WARN("[codex] auth probe failed: " + error);
        return false;
    }
    auto account = client.read_account(false, &error);
    if (!account.has_value()) {
        LOG_WARN("[codex] account/read failed: " + error);
        return false;
    }
    return account->present;
}

ChatResponse CodexProvider::chat(
    const std::vector<ChatMessage>& messages,
    const std::vector<ToolDef>& tools
) {
    ChatResponse response;
    response.finish_reason = "stop";
    chat_stream(messages, tools, [&](const StreamEvent& evt) {
        if (evt.type == StreamEventType::Delta) {
            response.content += evt.content;
        } else if (evt.type == StreamEventType::ReasoningDelta) {
            response.reasoning_content += evt.content;
        } else if (evt.type == StreamEventType::Usage) {
            response.usage = evt.usage;
        } else if (evt.type == StreamEventType::Error) {
            response.content = "[Error] " + evt.error;
            response.finish_reason = "error";
        }
    });
    return response;
}

void CodexProvider::chat_stream(
    const std::vector<ChatMessage>& messages,
    const std::vector<ToolDef>& tools,
    const StreamCallback& callback,
    std::atomic<bool>* abort_flag
) {
    if (!tools.empty()) {
        LOG_WARN("[codex] ACECode tool definitions are not forwarded to app-server; "
                 "Codex app-server owns its own tool runtime");
    }

    codex::AppServerClient client;
    std::string error;
    if (!client.start(&error) || !client.initialize(&error)) {
        emit_error(callback, model_, "Codex app-server unavailable: " + error);
        return;
    }

    auto account = client.read_account(false, &error);
    if (!account.has_value() || !account->present) {
        emit_error(callback, model_,
                   "Codex account is not logged in. Run `acecode configure` and select Codex.");
        return;
    }

    std::mutex mu;
    std::condition_variable cv;
    bool completed = false;
    bool failed = false;
    std::string failure_message;
    TokenUsage last_usage;

    client.set_notification_handler([&](const std::string& method, const nlohmann::json& params) {
        if (method == "item/agentMessage/delta") {
            std::string delta;
            if (params.contains("delta") && params["delta"].is_string()) {
                delta = params["delta"].get<std::string>();
            }
            if (!delta.empty()) {
                StreamEvent evt;
                evt.type = StreamEventType::Delta;
                evt.content = std::move(delta);
                callback(evt);
            }
            return;
        }
        if (method == "item/reasoning/summaryTextDelta") {
            std::string delta;
            if (params.contains("delta") && params["delta"].is_string()) {
                delta = params["delta"].get<std::string>();
            }
            if (!delta.empty()) {
                StreamEvent evt;
                evt.type = StreamEventType::ReasoningDelta;
                evt.content = std::move(delta);
                callback(evt);
            }
            return;
        }
        if (method == "thread/tokenUsage/updated" &&
            params.contains("tokenUsage") && params["tokenUsage"].is_object()) {
            const auto& usage = params["tokenUsage"];
            const auto* source = &usage;
            if (usage.contains("last") && usage["last"].is_object()) {
                source = &usage["last"];
            }
            TokenUsage parsed;
            parsed.prompt_tokens = source->value("inputTokens", 0);
            parsed.completion_tokens = source->value("outputTokens", 0);
            parsed.reasoning_tokens = source->value("reasoningOutputTokens", 0);
            parsed.cache_read_tokens = source->value("cachedInputTokens", 0);
            parsed.total_tokens = source->value("totalTokens", 0);
            parsed.has_data = true;
            std::lock_guard<std::mutex> lk(mu);
            last_usage = parsed;
            return;
        }
        if (method == "turn/completed") {
            std::lock_guard<std::mutex> lk(mu);
            completed = true;
            if (params.contains("turn") && params["turn"].is_object()) {
                std::string status = params["turn"].value("status", std::string{});
                if (status != "completed") {
                    failed = true;
                    failure_message = turn_error_message(params["turn"]);
                }
            }
            cv.notify_all();
        }
    });

    auto thread_id = client.start_thread(model_, current_cwd_utf8(), &error);
    if (!thread_id.has_value()) {
        emit_error(callback, model_, "Codex thread/start failed: " + error);
        return;
    }

    const std::string input_text = build_codex_input_text(messages);
    auto turn_id = client.start_turn(*thread_id, model_, current_cwd_utf8(), input_text, &error);
    if (!turn_id.has_value()) {
        emit_error(callback, model_, "Codex turn/start failed: " + error);
        return;
    }

    std::unique_lock<std::mutex> lk(mu);
    while (!completed) {
        if (abort_flag && abort_flag->load()) {
            failed = true;
            failure_message = "Request cancelled";
            break;
        }
        cv.wait_for(lk, std::chrono::milliseconds(100));
        if (!client.running()) {
            failed = true;
            failure_message = "Codex app-server stopped before turn completion";
            break;
        }
    }

    if (last_usage.has_data) {
        StreamEvent usage_evt;
        usage_evt.type = StreamEventType::Usage;
        usage_evt.usage = last_usage;
        lk.unlock();
        callback(usage_evt);
        lk.lock();
    }

    if (failed) {
        std::string message = failure_message.empty() ? "Codex turn failed" : failure_message;
        lk.unlock();
        emit_error(callback, model_, message);
        return;
    }

    lk.unlock();
    StreamEvent done;
    done.type = StreamEventType::Done;
    callback(done);
}

} // namespace acecode
