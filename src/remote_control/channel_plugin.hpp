#pragma once

#include "hooks/hook_config.hpp"
#include "hooks/hook_runner.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace acecode::rc {

inline constexpr int kChannelPluginProtocolVersion = 1;
inline constexpr const char* kChannelPluginSchema = "acecode.channel-plugin.v1";
inline constexpr const char* kRemoteControlTokenHeader = "X-ACECode-RC-Token";

struct ChannelPluginManifest {
    std::string name;
    std::string schema;
    std::string transport = "stdio";
    std::string command;
    std::vector<std::string> args;
    std::string cwd;
    int timeout_ms = 10000;
};

struct ChannelActivationRequest {
    int protocol_version = kChannelPluginProtocolVersion;
    std::string session_id;
    std::string inbound_url;
    std::string token_header = kRemoteControlTokenHeader;
    std::string token;
    std::string outbound_preference = "webhook";
    nlohmann::json settings = nlohmann::json::object();
};

struct ChannelPluginStatus {
    std::string state;
    bool already_running = false;
    std::string outbound_mode;
    std::string outbound_url;
    std::string message;

    bool connected() const { return state == "connected"; }
    bool failed() const { return state == "failed"; }
};

struct ChannelPluginActivationResult {
    bool ok = false;
    ChannelPluginStatus status;
};

bool parse_channel_plugin_manifest_json(const nlohmann::json& j,
                                        ChannelPluginManifest* out,
                                        std::string* error = nullptr);
std::optional<ChannelPluginManifest> load_channel_plugin_manifest(
    const std::string& path,
    std::string* error = nullptr);

nlohmann::json channel_activation_request_to_json(const ChannelActivationRequest& request);
nlohmann::json channel_deactivation_request_to_json(const std::string& session_id);

bool parse_channel_plugin_status_json(const nlohmann::json& j,
                                      ChannelPluginStatus* out,
                                      std::string* error = nullptr);

class ChannelPluginHost {
public:
    using Runner = std::function<HookProcessResult(const HookCommandSpec& command,
                                                   const std::string& stdin_text,
                                                   int timeout_ms,
                                                   const std::string& cwd)>;

    explicit ChannelPluginHost(Runner runner = default_runner());

    ChannelPluginActivationResult activate(const ChannelPluginManifest& manifest,
                                           const ChannelActivationRequest& request,
                                           int timeout_ms,
                                           std::string* error = nullptr) const;

    bool deactivate(const ChannelPluginManifest& manifest,
                    const std::string& session_id,
                    int timeout_ms,
                    std::string* error = nullptr) const;

    static Runner default_runner();

private:
    Runner runner_;
};

} // namespace acecode::rc
