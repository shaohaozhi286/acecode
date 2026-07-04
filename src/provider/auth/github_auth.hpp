#pragma once

#include <string>
#include <functional>
#include <vector>

namespace acecode {

struct DeviceCodeResponse {
    std::string device_code;
    std::string user_code;
    std::string verification_uri;
    int interval = 5; // polling interval in seconds
    int expires_in = 900;
};

struct CopilotToken {
    std::string token;
    int64_t expires_at = 0; // unix timestamp
};

struct DevicePollResult {
    // "pending", "slow_down", "authorized", "expired", or "failed".
    std::string status;
    std::string access_token;
    std::string error;
    std::string message;
    int interval_delta_seconds = 0;
};

struct CopilotModelsResult {
    std::vector<std::string> models;
    int status_code = 0;
    std::string error;
    std::string message;
};

// Step 1: Request a device code for GitHub OAuth Device Flow
DeviceCodeResponse request_device_code();

// Poll GitHub OAuth once for a device-code access token. This is intended for
// UI-driven polling where the caller owns the timer.
DevicePollResult poll_for_access_token_once(const std::string& device_code);

// Step 2: Poll for access token. Returns the GitHub OAuth token (gho_...) on success.
// Calls status_callback periodically with a status string for UI updates.
// Returns empty string on failure/expiry.
std::string poll_for_access_token(
    const std::string& device_code,
    int interval,
    int expires_in,
    std::function<void(const std::string&)> status_callback = nullptr
);

// Step 3: Exchange GitHub OAuth token for Copilot session token
CopilotToken exchange_copilot_token(const std::string& github_token);

// Fetch chat model ids from GitHub Copilot after exchanging the saved GitHub
// OAuth token for a Copilot session token.
CopilotModelsResult fetch_copilot_model_ids(const std::string& github_token);

// Persistence: save/load GitHub OAuth token from ~/.acecode/github_token
void save_github_token(const std::string& token);
std::string load_github_token();
bool has_saved_github_token();
bool delete_github_token(std::string* error = nullptr);

} // namespace acecode
