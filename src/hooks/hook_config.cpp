#include "hook_config.hpp"

#include "../config/config.hpp"
#include "../utils/encoding.hpp"
#include "../utils/utf8_path.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace acecode {
namespace {

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

HookMode parse_hook_mode(const nlohmann::json& obj) {
    if (!obj.contains("mode") || !obj["mode"].is_string()) return HookMode::Sync;
    return lower_ascii(obj["mode"].get<std::string>()) == "async"
        ? HookMode::Async
        : HookMode::Sync;
}

std::vector<std::string> parse_string_array(const nlohmann::json& value) {
    std::vector<std::string> out;
    if (!value.is_array()) return out;
    for (const auto& item : value) {
        if (item.is_string()) out.push_back(item.get<std::string>());
    }
    return out;
}

HookCommandSpec parse_command_spec(const nlohmann::json& obj) {
    HookCommandSpec spec;
    if (!obj.is_object()) return spec;
    if (obj.contains("command") && obj["command"].is_string()) {
        spec.command = obj["command"].get<std::string>();
    }
    if (obj.contains("args")) {
        spec.args = parse_string_array(obj["args"]);
    }
    return spec;
}

} // namespace

HookPlatform current_hook_platform() {
#if defined(_WIN32)
    return HookPlatform::Windows;
#elif defined(__APPLE__)
    return HookPlatform::Mac;
#else
    return HookPlatform::Linux;
#endif
}

std::string hook_platform_name(HookPlatform platform) {
    switch (platform) {
    case HookPlatform::Windows: return "windows";
    case HookPlatform::Linux: return "linux";
    case HookPlatform::Mac: return "mac";
    }
    return "unknown";
}

std::vector<std::string> hook_platform_command_resolution_order(HookPlatform platform) {
    switch (platform) {
    case HookPlatform::Windows:
        return {"windows"};
    case HookPlatform::Linux:
        return {"linux", "unix", "posix"};
    case HookPlatform::Mac:
        return {"mac", "unix", "posix"};
    }
    return {};
}

std::string normalize_hook_platform_name(std::string value) {
    value = lower_ascii(std::move(value));
    if (value == "darwin" || value == "macos" || value == "osx") return "mac";
    if (value == "win" || value == "win32") return "windows";
    return value;
}

bool hook_platform_matches(const HookDefinition& hook, HookPlatform current) {
    if (hook.platforms.empty()) return true;
    auto order = hook_platform_command_resolution_order(current);
    for (const auto& raw : hook.platforms) {
        std::string p = normalize_hook_platform_name(raw);
        if (std::find(order.begin(), order.end(), p) != order.end()) return true;
    }
    return false;
}

std::optional<HookCommandSpec> resolve_hook_command(const HookDefinition& hook,
                                                    HookPlatform current) {
    for (const auto& key : hook_platform_command_resolution_order(current)) {
        auto it = hook.commands_by_platform.find(key);
        if (it != hook.commands_by_platform.end() && it->second.valid()) {
            return it->second;
        }
    }
    if (hook.command.valid()) return hook.command;
    return std::nullopt;
}

HookConfig parse_hook_config_json(const nlohmann::json& j, std::string* error) {
    HookConfig cfg;
    if (error) error->clear();
    if (!j.is_object()) {
        if (error) *error = "hooks config root must be an object";
        return cfg;
    }

    if (j.contains("version") && j["version"].is_number_integer()) {
        cfg.version = j["version"].get<int>();
    }
    if (j.contains("enabled") && j["enabled"].is_boolean()) {
        cfg.enabled = j["enabled"].get<bool>();
    }
    if (!j.contains("events")) return cfg;
    if (!j["events"].is_object()) {
        if (error) *error = "hooks.events must be an object";
        cfg.enabled = false;
        return cfg;
    }

    for (auto it = j["events"].begin(); it != j["events"].end(); ++it) {
        const std::string event_name = it.key();
        if (!it.value().is_array()) continue;

        auto& hooks = cfg.events[event_name];
        int index = 0;
        for (const auto& item : it.value()) {
            ++index;
            if (!item.is_object()) continue;

            HookDefinition hook;
            hook.event = event_name;
            hook.id = item.value("id", event_name + "#" + std::to_string(index));
            hook.mode = parse_hook_mode(item);
            if (item.contains("timeout_ms") && item["timeout_ms"].is_number_integer()) {
                hook.timeout_ms = item["timeout_ms"].get<int>();
            }
            if (item.contains("platforms")) {
                hook.platforms = parse_string_array(item["platforms"]);
                for (auto& platform : hook.platforms) {
                    platform = normalize_hook_platform_name(std::move(platform));
                }
            }

            hook.command = parse_command_spec(item);
            if (item.contains("commands") && item["commands"].is_object()) {
                for (auto cit = item["commands"].begin(); cit != item["commands"].end(); ++cit) {
                    HookCommandSpec spec = parse_command_spec(cit.value());
                    if (spec.valid()) {
                        hook.commands_by_platform[normalize_hook_platform_name(cit.key())] =
                            std::move(spec);
                    }
                }
            }

            if (hook.command.valid() || !hook.commands_by_platform.empty()) {
                hooks.push_back(std::move(hook));
            }
        }
    }

    return cfg;
}

HookConfig load_hook_config_from_path(const std::string& path, std::string* error) {
    if (error) error->clear();
    if (path.empty()) return HookConfig{};

    fs::path native = path_from_utf8(path);
    std::error_code ec;
    if (!fs::is_regular_file(native, ec)) return HookConfig{};

    std::ifstream ifs(native, std::ios::binary);
    if (!ifs.is_open()) {
        if (error) *error = "failed to open hooks config: " + path;
        return HookConfig{};
    }

    try {
        nlohmann::json j = nlohmann::json::parse(ifs);
        return parse_hook_config_json(j, error);
    } catch (const std::exception& e) {
        if (error) *error = std::string("failed to parse hooks config: ") + e.what();
        return HookConfig{};
    }
}

std::string default_hook_config_path() {
    return path_to_utf8(path_from_utf8(get_acecode_dir()) / "hooks.json");
}

HookConfig load_hook_config(std::string* error) {
    return load_hook_config_from_path(default_hook_config_path(), error);
}

} // namespace acecode
