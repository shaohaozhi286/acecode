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

struct HostStartResult {
    bool ok = false;
    std::string error;
};

using CliRunner = std::function<CliProcessResult(
    const std::vector<std::string>& argv,
    const std::string& stdin_text,
    std::chrono::milliseconds timeout)>;

using HostStarter = std::function<HostStartResult(const std::vector<std::string>& argv)>;

class AceBrowserBridgeClient {
public:
    explicit AceBrowserBridgeClient(AceBrowserBridgeConfig config,
                                    CliRunner runner = CliRunner{},
                                    HostStarter host_starter = HostStarter{});

    BridgeEnvelope status();
    BridgeEnvelope ensure_ready();
    BridgeEnvelope command(const BrowserCommandRequest& request);
    BridgeEnvelope screenshot(const std::string& session, const std::string& output_path);

    void clear_status_cache();

    static BridgeEnvelope parse_envelope(const std::string& stdout_text);
    static nlohmann::json command_request_json(const BrowserCommandRequest& request);

private:
    BridgeEnvelope run_json_command(const std::vector<std::string>& args,
                                    const std::string& stdin_text);
    BridgeEnvelope status_once();
    BridgeEnvelope ensure_host_running_from_status(BridgeEnvelope status_envelope);
    HostStartResult start_host_daemon();
    bool should_cache_status(const BridgeEnvelope& envelope) const;

    AceBrowserBridgeConfig config_;
    CliRunner runner_;
    HostStarter host_starter_;
    std::optional<BridgeEnvelope> cached_status_;
    std::chrono::steady_clock::time_point cached_status_at_{};
};

CliProcessResult run_cli_process(const std::vector<std::string>& argv,
                                 const std::string& stdin_text,
                                 std::chrono::milliseconds timeout);

HostStartResult start_host_process(const std::vector<std::string>& argv);

} // namespace acecode::ace_browser_bridge
