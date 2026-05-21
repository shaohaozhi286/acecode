#pragma once

#include "config/config.hpp"

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace acecode::ace_browser_bridge {

struct BridgeError {
    std::string code;
    std::string message;
};

struct BridgeEnvelope {
    bool ok = false;
    nlohmann::json data = nlohmann::json::object();
    std::optional<BridgeError> error;
};

struct BrowserCommandRequest {
    std::string session;
    std::string action;
    nlohmann::json args = nlohmann::json::object();
};

struct CliProcessResult {
    int exit_code = -1;
    bool timed_out = false;
    std::string stdout_text;
    std::string error;
};

using CliRunner = std::function<CliProcessResult(
    const std::vector<std::string>& argv,
    const std::string& stdin_text,
    std::chrono::milliseconds timeout)>;

class AceBrowserBridgeClient {
public:
    explicit AceBrowserBridgeClient(AceBrowserBridgeConfig config,
                                    CliRunner runner = CliRunner{});

    BridgeEnvelope status();
    BridgeEnvelope command(const BrowserCommandRequest& request);
    BridgeEnvelope screenshot(const std::string& session, const std::string& output_path);

    void clear_status_cache();

    static BridgeEnvelope parse_envelope(const std::string& stdout_text);
    static nlohmann::json command_request_json(const BrowserCommandRequest& request);

private:
    BridgeEnvelope run_json_command(const std::vector<std::string>& args,
                                    const std::string& stdin_text);
    bool should_cache_status(const BridgeEnvelope& envelope) const;

    AceBrowserBridgeConfig config_;
    CliRunner runner_;
    std::optional<BridgeEnvelope> cached_status_;
    std::chrono::steady_clock::time_point cached_status_at_{};
};

CliProcessResult run_cli_process(const std::vector<std::string>& argv,
                                 const std::string& stdin_text,
                                 std::chrono::milliseconds timeout);

} // namespace acecode::ace_browser_bridge
