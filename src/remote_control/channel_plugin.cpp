#include "channel_plugin.hpp"

#include "utils/utf8_path.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace acecode::rc {

namespace {

void set_error(std::string* error, std::string message) {
    if (error) *error = std::move(message);
}

bool is_http_url(const std::string& value) {
    return value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0;
}

bool read_string_field(const nlohmann::json& j,
                       const char* key,
                       std::string* out,
                       bool required,
                       std::string* error) {
    if (!j.contains(key)) {
        if (required) set_error(error, std::string("missing required field: ") + key);
        return !required;
    }
    if (!j[key].is_string()) {
        set_error(error, std::string("field must be a string: ") + key);
        return false;
    }
    *out = j[key].get<std::string>();
    if (required && out->empty()) {
        set_error(error, std::string("field must not be empty: ") + key);
        return false;
    }
    return true;
}

bool read_args_field(const nlohmann::json& j,
                     std::vector<std::string>* out,
                     std::string* error) {
    out->clear();
    if (!j.contains("args")) return true;
    if (!j["args"].is_array()) {
        set_error(error, "field must be an array: args");
        return false;
    }
    for (const auto& item : j["args"]) {
        if (!item.is_string()) {
            set_error(error, "args entries must be strings");
            return false;
        }
        out->push_back(item.get<std::string>());
    }
    return true;
}

std::string runner_error(const HookProcessResult& result) {
    if (!result.error.empty()) return result.error;
    if (!result.stderr_text.empty()) return result.stderr_text;
    if (!result.output.empty()) return result.output;
    return "plugin process failed";
}

int effective_timeout_ms(const ChannelPluginManifest& manifest, int override_ms) {
    if (override_ms > 0) return override_ms;
    return manifest.timeout_ms > 0 ? manifest.timeout_ms : 10000;
}

bool validate_stdio_manifest(const ChannelPluginManifest& manifest,
                             std::string* error) {
    if (manifest.transport != "stdio") {
        set_error(error, "unsupported channel plugin transport: " + manifest.transport);
        return false;
    }
    if (manifest.command.empty()) {
        set_error(error, "channel plugin command is empty");
        return false;
    }
    return true;
}

} // namespace

bool parse_channel_plugin_manifest_json(const nlohmann::json& j,
                                        ChannelPluginManifest* out,
                                        std::string* error) {
    if (error) error->clear();
    if (!out) {
        set_error(error, "output pointer is null");
        return false;
    }
    if (!j.is_object()) {
        set_error(error, "channel plugin manifest must be a JSON object");
        return false;
    }

    ChannelPluginManifest manifest;
    if (!read_string_field(j, "name", &manifest.name, true, error)) return false;
    if (j.contains("schema")) {
        if (!read_string_field(j, "schema", &manifest.schema, false, error)) return false;
        if (!manifest.schema.empty() && manifest.schema != kChannelPluginSchema) {
            set_error(error, "unsupported channel plugin schema: " + manifest.schema);
            return false;
        }
    } else {
        manifest.schema = kChannelPluginSchema;
    }
    if (j.contains("transport") &&
        !read_string_field(j, "transport", &manifest.transport, false, error)) {
        return false;
    }

    const nlohmann::json* launch = &j;
    if (!j.contains("command") && j.contains("launcher") && j["launcher"].is_object()) {
        launch = &j["launcher"];
    }

    if (!read_string_field(*launch, "command", &manifest.command, true, error)) {
        return false;
    }
    if (!read_args_field(*launch, &manifest.args, error)) return false;
    if (launch->contains("cwd") &&
        !read_string_field(*launch, "cwd", &manifest.cwd, false, error)) {
        return false;
    }
    if (j.contains("cwd") && manifest.cwd.empty() &&
        !read_string_field(j, "cwd", &manifest.cwd, false, error)) {
        return false;
    }
    if (j.contains("timeout_ms")) {
        if (!j["timeout_ms"].is_number_integer()) {
            set_error(error, "field must be an integer: timeout_ms");
            return false;
        }
        manifest.timeout_ms = j["timeout_ms"].get<int>();
        if (manifest.timeout_ms < 1000 || manifest.timeout_ms > 120000) {
            set_error(error, "timeout_ms out of range (1000-120000)");
            return false;
        }
    }
    if (!validate_stdio_manifest(manifest, error)) return false;

    *out = std::move(manifest);
    return true;
}

std::optional<ChannelPluginManifest> load_channel_plugin_manifest(const std::string& path,
                                                                  std::string* error) {
    if (error) error->clear();
    std::ifstream ifs(path_from_utf8(path));
    if (!ifs.is_open()) {
        set_error(error, "failed to open channel plugin manifest: " + path);
        return std::nullopt;
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(ifs);
    } catch (const std::exception& e) {
        set_error(error, std::string("failed to parse channel plugin manifest: ") + e.what());
        return std::nullopt;
    }

    ChannelPluginManifest manifest;
    if (!parse_channel_plugin_manifest_json(j, &manifest, error)) return std::nullopt;
    return manifest;
}

nlohmann::json channel_activation_request_to_json(const ChannelActivationRequest& request) {
    return nlohmann::json{
        {"type", "channel.activate"},
        {"protocol_version", request.protocol_version},
        {"session_id", request.session_id},
        {"inbound",
         nlohmann::json{
             {"url", request.inbound_url},
             {"token_header", request.token_header},
             {"token", request.token},
         }},
        {"outbound",
         nlohmann::json{
             {"preferred", request.outbound_preference},
         }},
        {"settings", request.settings.is_object() ? request.settings : nlohmann::json::object()},
    };
}

nlohmann::json channel_deactivation_request_to_json(const std::string& session_id) {
    return nlohmann::json{
        {"type", "channel.deactivate"},
        {"protocol_version", kChannelPluginProtocolVersion},
        {"session_id", session_id},
    };
}

bool parse_channel_plugin_status_json(const nlohmann::json& j,
                                      ChannelPluginStatus* out,
                                      std::string* error) {
    if (error) error->clear();
    if (!out) {
        set_error(error, "output pointer is null");
        return false;
    }
    if (!j.is_object()) {
        set_error(error, "channel plugin status must be a JSON object");
        return false;
    }
    if (!j.contains("type") || !j["type"].is_string() ||
        j["type"].get<std::string>() != "channel.status") {
        set_error(error, "plugin response type must be channel.status");
        return false;
    }

    ChannelPluginStatus status;
    if (!read_string_field(j, "state", &status.state, true, error)) return false;
    if (status.state != "connected" && status.state != "pending" &&
        status.state != "failed") {
        set_error(error, "unsupported channel status state: " + status.state);
        return false;
    }
    if (j.contains("already_running")) {
        if (!j["already_running"].is_boolean()) {
            set_error(error, "field must be a boolean: already_running");
            return false;
        }
        status.already_running = j["already_running"].get<bool>();
    }
    if (j.contains("message")) {
        if (!j["message"].is_string()) {
            set_error(error, "field must be a string: message");
            return false;
        }
        status.message = j["message"].get<std::string>();
    }
    if (j.contains("outbound")) {
        if (!j["outbound"].is_object()) {
            set_error(error, "field must be an object: outbound");
            return false;
        }
        const auto& outbound = j["outbound"];
        if (outbound.contains("mode") &&
            !read_string_field(outbound, "mode", &status.outbound_mode, false, error)) {
            return false;
        }
        if (outbound.contains("url") &&
            !read_string_field(outbound, "url", &status.outbound_url, false, error)) {
            return false;
        }
    }
    *out = std::move(status);
    return true;
}

ChannelPluginHost::ChannelPluginHost(Runner runner)
    : runner_(std::move(runner)) {
    if (!runner_) runner_ = default_runner();
}

ChannelPluginHost::Runner ChannelPluginHost::default_runner() {
    return [](const HookCommandSpec& command,
              const std::string& stdin_text,
              int timeout_ms,
              const std::string& cwd) {
        return run_hook_process(command, stdin_text, timeout_ms, cwd);
    };
}

ChannelPluginActivationResult ChannelPluginHost::activate(
    const ChannelPluginManifest& manifest,
    const ChannelActivationRequest& request,
    int timeout_ms,
    std::string* error) const {
    if (error) error->clear();
    ChannelPluginActivationResult activation;
    if (!validate_stdio_manifest(manifest, error)) return activation;

    HookCommandSpec command{manifest.command, manifest.args};
    const std::string stdin_text = channel_activation_request_to_json(request).dump() + "\n";
    HookProcessResult result =
        runner_(command, stdin_text, effective_timeout_ms(manifest, timeout_ms), manifest.cwd);
    if (!result.started) {
        set_error(error, "failed to start channel plugin: " + runner_error(result));
        return activation;
    }
    if (result.timed_out) {
        set_error(error, "channel plugin timed out");
        return activation;
    }
    if (result.exit_code != 0) {
        set_error(error, "channel plugin exited with code " +
                             std::to_string(result.exit_code) + ": " + runner_error(result));
        return activation;
    }

    auto status_json = nlohmann::json::parse(result.stdout_text, nullptr, false);
    if (status_json.is_discarded()) {
        set_error(error, "channel plugin did not return valid JSON status");
        return activation;
    }
    if (!parse_channel_plugin_status_json(status_json, &activation.status, error)) {
        return activation;
    }
    if (activation.status.failed()) {
        set_error(error, activation.status.message.empty()
                             ? "channel plugin reported failed"
                             : activation.status.message);
        return activation;
    }
    if (!activation.status.connected()) {
        set_error(error, activation.status.message.empty()
                             ? "channel plugin did not report connected"
                             : activation.status.message);
        return activation;
    }
    if (activation.status.outbound_mode != "webhook") {
        set_error(error, "unsupported plugin outbound mode: " +
                             activation.status.outbound_mode);
        return activation;
    }
    if (!is_http_url(activation.status.outbound_url)) {
        set_error(error, "plugin outbound webhook URL must start with http:// or https://");
        return activation;
    }

    activation.ok = true;
    return activation;
}

bool ChannelPluginHost::deactivate(const ChannelPluginManifest& manifest,
                                   const std::string& session_id,
                                   int timeout_ms,
                                   std::string* error) const {
    if (error) error->clear();
    if (!validate_stdio_manifest(manifest, error)) return false;

    HookCommandSpec command{manifest.command, manifest.args};
    const std::string stdin_text = channel_deactivation_request_to_json(session_id).dump() + "\n";
    HookProcessResult result =
        runner_(command, stdin_text, effective_timeout_ms(manifest, timeout_ms), manifest.cwd);
    if (!result.started) {
        set_error(error, "failed to start channel plugin: " + runner_error(result));
        return false;
    }
    if (result.timed_out) {
        set_error(error, "channel plugin timed out");
        return false;
    }
    if (result.exit_code != 0) {
        set_error(error, "channel plugin exited with code " +
                             std::to_string(result.exit_code) + ": " + runner_error(result));
        return false;
    }
    if (result.stdout_text.empty()) return true;

    auto status_json = nlohmann::json::parse(result.stdout_text, nullptr, false);
    if (status_json.is_discarded()) return true;

    ChannelPluginStatus status;
    if (!parse_channel_plugin_status_json(status_json, &status, error)) return false;
    if (status.failed()) {
        set_error(error, status.message.empty()
                             ? "channel plugin reported failed"
                             : status.message);
        return false;
    }
    return true;
}

} // namespace acecode::rc
