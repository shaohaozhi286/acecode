#include "github_auth.hpp"
#include "../../config/config.hpp"
#include "../../network/proxy_resolver.hpp"
#include "../../utils/utf8_path.hpp"

#include <cpr/cpr.h>
#include <cpr/ssl_options.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <thread>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <set>
#include <utility>

namespace fs = std::filesystem;

namespace acecode {

// GitHub OAuth App client_id for Copilot CLI integrations.
// This is the well-known public client_id used by open-source Copilot clients.
static const std::string GITHUB_CLIENT_ID = "Iv1.b507a08c87ecfe98";
static const std::string ACCESS_TOKEN_URL =
    "https://github.com/login/oauth/access_token";
static const std::string COPILOT_TOKEN_URL =
    "https://api.github.com/copilot_internal/v2/token";
static const std::string COPILOT_MODELS_URL =
    "https://api.githubcopilot.com/models";

DeviceCodeResponse request_device_code() {
    static const std::string kDeviceCodeUrl = "https://github.com/login/device/code";
    auto proxy_opts = network::proxy_options_for(kDeviceCodeUrl);
    cpr::Response r = cpr::Post(
        cpr::Url{kDeviceCodeUrl},
        cpr::Header{
            {"Accept", "application/json"},
            {"Content-Type", "application/json"}
        },
        cpr::Body{nlohmann::json({
            {"client_id", GITHUB_CLIENT_ID},
            {"scope", "read:user"}
        }).dump()},
        network::build_ssl_options(proxy_opts),
        proxy_opts.proxies,
        proxy_opts.auth,
        cpr::Timeout{30000}
    );

    DeviceCodeResponse result;
    if (r.status_code != 200) {
        std::cerr << "[auth] Device code request failed: HTTP " << r.status_code
                  << ", error: " << r.error.message << std::endl;
        return result;
    }

    try {
        auto j = nlohmann::json::parse(r.text);
        result.device_code = j.value("device_code", "");
        result.user_code = j.value("user_code", "");
        result.verification_uri = j.value("verification_uri", "https://github.com/login/device");
        result.interval = j.value("interval", 5);
        result.expires_in = j.value("expires_in", 900);
    } catch (...) {}

    return result;
}

std::string poll_for_access_token(
    const std::string& device_code,
    int interval,
    int expires_in,
    std::function<void(const std::string&)> status_callback
) {
    auto start = std::chrono::steady_clock::now();
    int poll_interval = interval;

    while (true) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start
        ).count();

        if (elapsed >= expires_in) {
            if (status_callback) status_callback("Device code expired.");
            return "";
        }

        std::this_thread::sleep_for(std::chrono::seconds(poll_interval));

        DevicePollResult poll = poll_for_access_token_once(device_code);
        if (poll.status == "authorized") {
            return poll.access_token;
        }
        if (poll.status == "pending") {
            if (status_callback) status_callback("Waiting for authorization...");
            continue;
        }
        if (poll.status == "slow_down") {
            poll_interval += poll.interval_delta_seconds > 0
                ? poll.interval_delta_seconds
                : 5;
            if (status_callback) status_callback("Slowing down polling...");
            continue;
        }
        if (poll.status == "expired") {
            if (status_callback) status_callback("Device code expired.");
            return "";
        }
        if (status_callback) {
            status_callback(poll.message.empty()
                ? "Auth error: " + poll.error
                : poll.message);
        }
        return "";
    }
}

CopilotToken exchange_copilot_token(const std::string& github_token) {
    auto proxy_opts = network::proxy_options_for(COPILOT_TOKEN_URL);
    cpr::Response r = cpr::Get(
        cpr::Url{COPILOT_TOKEN_URL},
        cpr::Header{
            {"Authorization", "token " + github_token},
            {"Accept", "application/json"},
            {"User-Agent", "acecode/0.1.0"}
        },
        network::build_ssl_options(proxy_opts),
        proxy_opts.proxies,
        proxy_opts.auth,
        cpr::Timeout{30000}
    );

    CopilotToken result;
    if (r.status_code != 200) {
        return result;
    }

    try {
        auto j = nlohmann::json::parse(r.text);
        result.token = j.value("token", "");
        result.expires_at = j.value("expires_at", (int64_t)0);
    } catch (...) {}

    return result;
}

DevicePollResult poll_for_access_token_once(const std::string& device_code) {
    DevicePollResult result;
    if (device_code.empty()) {
        result.status = "failed";
        result.error = "missing_device_code";
        result.message = "device_code is required";
        return result;
    }

    auto proxy_opts = network::proxy_options_for(ACCESS_TOKEN_URL);
    cpr::Response r = cpr::Post(
        cpr::Url{ACCESS_TOKEN_URL},
        cpr::Header{
            {"Accept", "application/json"},
            {"Content-Type", "application/json"}
        },
        cpr::Body{nlohmann::json({
            {"client_id", GITHUB_CLIENT_ID},
            {"device_code", device_code},
            {"grant_type", "urn:ietf:params:oauth:grant-type:device_code"}
        }).dump()},
        network::build_ssl_options(proxy_opts),
        proxy_opts.proxies,
        proxy_opts.auth,
        cpr::Timeout{30000}
    );

    if (r.status_code != 200) {
        result.status = "failed";
        result.error = "http_error";
        result.message = "HTTP error during polling";
        return result;
    }

    try {
        auto j = nlohmann::json::parse(r.text);
        if (j.contains("access_token") && j["access_token"].is_string()) {
            result.status = "authorized";
            result.access_token = j["access_token"].get<std::string>();
            return result;
        }

        std::string error = j.value("error", "");
        result.error = error;
        if (error == "authorization_pending") {
            result.status = "pending";
            result.message = "Waiting for authorization";
        } else if (error == "slow_down") {
            result.status = "slow_down";
            result.interval_delta_seconds = 5;
            result.message = "Slow down polling";
        } else if (error == "expired_token") {
            result.status = "expired";
            result.message = "Device code expired";
        } else {
            result.status = "failed";
            result.message = error.empty() ? "Authentication failed" : "Auth error: " + error;
        }
    } catch (const std::exception& e) {
        result.status = "failed";
        result.error = "bad_json";
        result.message = e.what();
    }
    return result;
}

CopilotModelsResult fetch_copilot_model_ids(const std::string& github_token) {
    CopilotModelsResult result;
    if (github_token.empty()) {
        result.status_code = 401;
        result.error = "COPILOT_AUTH_REQUIRED";
        result.message = "GitHub Copilot authentication is required";
        return result;
    }

    CopilotToken ct = exchange_copilot_token(github_token);
    if (ct.token.empty()) {
        result.status_code = 401;
        result.error = "COPILOT_TOKEN_EXCHANGE_FAILED";
        result.message = "Could not exchange GitHub token for Copilot session token";
        return result;
    }

    auto proxy_opts = network::proxy_options_for(COPILOT_MODELS_URL);
    cpr::Response r = cpr::Get(
        cpr::Url{COPILOT_MODELS_URL},
        cpr::Header{
            {"Authorization", "Bearer " + ct.token},
            {"Editor-Version", "acecode/0.1.0"},
            {"Editor-Plugin-Version", "acecode/0.1.0"},
            {"Copilot-Integration-Id", "vscode-chat"},
            {"Openai-Intent", "conversation-panel"}
        },
        network::build_ssl_options(proxy_opts),
        proxy_opts.proxies,
        proxy_opts.auth,
        cpr::Timeout{10000}
    );

    result.status_code = static_cast<int>(r.status_code);
    if (r.status_code == 0) {
        result.error = "COPILOT_MODELS_UNREACHABLE";
        result.message = r.error.message;
        return result;
    }
    if (r.status_code < 200 || r.status_code >= 300) {
        result.error = "COPILOT_MODELS_HTTP_ERROR";
        result.message = "upstream returned HTTP " + std::to_string(r.status_code);
        return result;
    }

    try {
        auto j = nlohmann::json::parse(r.text);
        const nlohmann::json* list = nullptr;
        if (j.is_object()) {
            if (j.contains("data") && j["data"].is_array()) {
                list = &j["data"];
            } else if (j.contains("models") && j["models"].is_array()) {
                list = &j["models"];
            }
        } else if (j.is_array()) {
            list = &j;
        }

        std::set<std::string> unique;
        if (list) {
            for (const auto& item : *list) {
                if (item.is_string()) {
                    auto id = item.get<std::string>();
                    if (!id.empty()) unique.insert(std::move(id));
                    continue;
                }
                if (!item.is_object() ||
                    !item.contains("id") ||
                    !item["id"].is_string()) {
                    continue;
                }
                if (item.contains("capabilities") &&
                    item["capabilities"].is_object()) {
                    const auto& caps = item["capabilities"];
                    if (caps.contains("type") && caps["type"].is_string() &&
                        caps["type"].get<std::string>() != "chat") {
                        continue;
                    }
                }
                auto id = item["id"].get<std::string>();
                if (!id.empty()) unique.insert(std::move(id));
            }
        }
        result.models.assign(unique.begin(), unique.end());
    } catch (const std::exception& e) {
        result.error = "COPILOT_MODELS_BAD_JSON";
        result.message = e.what();
    }
    return result;
}

static fs::path github_token_path() {
    return path_from_utf8(get_acecode_dir()) / "github_token";
}

void save_github_token(const std::string& token) {
    std::string dir = get_acecode_dir();
    fs::path native_dir = path_from_utf8(dir);
    if (!fs::exists(native_dir)) {
        fs::create_directories(native_dir);
    }
    fs::path path = native_dir / "github_token";
    std::ofstream ofs(path);
    if (ofs.is_open()) {
        ofs << token;
    }
}

std::string load_github_token() {
    fs::path path = github_token_path();
    if (!fs::exists(path)) {
        return "";
    }
    std::ifstream ifs(path);
    std::string token;
    if (ifs.is_open()) {
        std::getline(ifs, token);
    }
    return token;
}

bool has_saved_github_token() {
    return !load_github_token().empty();
}

bool delete_github_token(std::string* error) {
    std::error_code ec;
    fs::remove(github_token_path(), ec);
    if (ec) {
        if (error) *error = ec.message();
        return false;
    }
    return true;
}

} // namespace acecode
