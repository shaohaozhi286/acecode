#include "copilot_provider.hpp"
#include "utils/logger.hpp"
#include <cpr/cpr.h>
#include <ctime>
#include <map>

namespace acecode {

static const std::string COPILOT_CHAT_URL = "https://api.githubcopilot.com/chat/completions";

CopilotProvider::CopilotProvider(const std::string& model)
    : OpenAiCompatProvider(COPILOT_CHAT_URL, "", model) {}

bool CopilotProvider::is_authenticated() {
    return !github_token_.empty() && ensure_copilot_token();
}

bool CopilotProvider::try_silent_auth() {
    github_token_ = load_github_token();
    if (github_token_.empty()) {
        return false;
    }
    return ensure_copilot_token();
}

bool CopilotProvider::run_device_flow(std::function<void(const std::string&)> status_callback) {
    device_code_ = request_device_code();
    if (device_code_.device_code.empty()) {
        if (status_callback) status_callback("Failed to request device code.");
        return false;
    }

    // The caller (TUI) should display device_code_.user_code and verification_uri
    // before we start polling.

    github_token_ = poll_for_access_token(
        device_code_.device_code,
        device_code_.interval,
        device_code_.expires_in,
        status_callback
    );

    if (github_token_.empty()) {
        return false;
    }

    save_github_token(github_token_);
    return ensure_copilot_token();
}

bool CopilotProvider::authenticate() {
    if (try_silent_auth()) {
        return true;
    }
    return run_device_flow();
}

bool CopilotProvider::ensure_copilot_token() {
    // Check if we already have a valid (non-expired) copilot token
    if (!copilot_token_.token.empty()) {
        int64_t now = static_cast<int64_t>(std::time(nullptr));
        if (now < copilot_token_.expires_at - 60) { // 60s margin
            LOG_DEBUG("Copilot token still valid, expires_at=" + std::to_string(copilot_token_.expires_at));
            return true;
        }
    }

    LOG_INFO("Exchanging copilot token...");
    // Exchange for a new copilot token
    copilot_token_ = exchange_copilot_token(github_token_);
    LOG_INFO("Copilot token exchange result: " + std::string(copilot_token_.token.empty() ? "FAILED" : "OK"));
    return !copilot_token_.token.empty();
}

ChatResponse CopilotProvider::chat(
    const std::vector<ChatMessage>& messages,
    const std::vector<ToolDef>& tools
) {
    if (!ensure_copilot_token()) {
        ChatResponse resp;
        resp.content = "[Error] Copilot session token unavailable. Re-authenticate.";
        resp.finish_reason = "error";
        return resp;
    }

    nlohmann::json body = build_request_body(messages, tools);

    cpr::Header headers = {
        {"Content-Type", "application/json"},
        {"Authorization", "Bearer " + copilot_token_.token},
        {"Editor-Version", "acecode/0.1.0"},
        {"Editor-Plugin-Version", "acecode/0.1.0"},
        {"Copilot-Integration-Id", "vscode-chat"},
        {"Openai-Intent", "conversation-panel"}
    };

    cpr::Response r = cpr::Post(
        cpr::Url{COPILOT_CHAT_URL},
        headers,
        cpr::Body{body.dump()},
        cpr::Timeout{120000}
    );

    if (r.status_code == 0) {
        ChatResponse resp;
        resp.content = "[Error] Connection failed: " + r.error.message;
        resp.finish_reason = "error";
        return resp;
    }

    if (r.status_code == 401) {
        // Token expired, try refresh once
        copilot_token_ = {};
        if (ensure_copilot_token()) {
            headers["Authorization"] = "Bearer " + copilot_token_.token;
            r = cpr::Post(
                cpr::Url{COPILOT_CHAT_URL},
                headers,
                cpr::Body{body.dump()},
                cpr::Timeout{120000}
            );
        }
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
        resp.content = "[Error] Failed to parse response: " + std::string(e.what());
        resp.finish_reason = "error";
        return resp;
    }
}

void CopilotProvider::chat_stream(
    const std::vector<ChatMessage>& messages,
    const std::vector<ToolDef>& tools,
    const StreamCallback& callback,
    std::atomic<bool>* abort_flag
) {
    LOG_INFO("CopilotProvider::chat_stream messages=" + std::to_string(messages.size()) + " tools=" + std::to_string(tools.size()));
    if (!ensure_copilot_token()) {
        LOG_ERROR("Copilot token unavailable for streaming");
        StreamEvent evt;
        evt.type = StreamEventType::Error;
        evt.error = "Copilot session token unavailable. Re-authenticate.";
        callback(evt);
        return;
    }

    nlohmann::json body = build_request_body(messages, tools, true);

    std::map<std::string, std::string> extra_headers = {
        {"Authorization", "Bearer " + copilot_token_.token},
        {"Editor-Version", "acecode/0.1.0"},
        {"Editor-Plugin-Version", "acecode/0.1.0"},
        {"Copilot-Integration-Id", "vscode-chat"},
        {"Openai-Intent", "conversation-panel"}
    };

    parse_sse_stream(COPILOT_CHAT_URL, body, extra_headers, callback, abort_flag);
}

} // namespace acecode
