#include "github_auth.hpp"
#include "../config/config.hpp"
#include "../network/proxy_resolver.hpp"

#include <cpr/cpr.h>
#include <cpr/ssl_options.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <thread>
#include <chrono>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

namespace acecode {

// GitHub OAuth App client_id for Copilot CLI integrations.
// This is the well-known public client_id used by open-source Copilot clients.
static const std::string GITHUB_CLIENT_ID = "Iv1.b507a08c87ecfe98";

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

        static const std::string kAccessTokenUrl =
            "https://github.com/login/oauth/access_token";
        auto proxy_opts = network::proxy_options_for(kAccessTokenUrl);
        cpr::Response r = cpr::Post(
            cpr::Url{kAccessTokenUrl},
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
            if (status_callback) status_callback("HTTP error during polling.");
            continue;
        }

        try {
            auto j = nlohmann::json::parse(r.text);

            if (j.contains("access_token")) {
                return j["access_token"].get<std::string>();
            }

            std::string error = j.value("error", "");
            if (error == "authorization_pending") {
                if (status_callback) status_callback("Waiting for authorization...");
                continue;
            } else if (error == "slow_down") {
                poll_interval += 5;
                if (status_callback) status_callback("Slowing down polling...");
                continue;
            } else if (error == "expired_token") {
                if (status_callback) status_callback("Device code expired.");
                return "";
            } else {
                if (status_callback) status_callback("Auth error: " + error);
                return "";
            }
        } catch (...) {
            if (status_callback) status_callback("Failed to parse polling response.");
        }
    }
}

CopilotToken exchange_copilot_token(const std::string& github_token) {
    static const std::string kCopilotTokenUrl =
        "https://api.github.com/copilot_internal/v2/token";
    auto proxy_opts = network::proxy_options_for(kCopilotTokenUrl);
    cpr::Response r = cpr::Get(
        cpr::Url{kCopilotTokenUrl},
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

void save_github_token(const std::string& token) {
    std::string dir = get_acecode_dir();
    if (!fs::exists(dir)) {
        fs::create_directories(dir);
    }
    std::string path = (fs::path(dir) / "github_token").string();
    std::ofstream ofs(path);
    if (ofs.is_open()) {
        ofs << token;
    }
}

std::string load_github_token() {
    std::string path = (fs::path(get_acecode_dir()) / "github_token").string();
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

} // namespace acecode
