#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace acecode {

inline constexpr const char* kHookEventStartupModelsLoaded = "startup.models_loaded";
inline constexpr const char* kHookEventStartupBeforeModelLoad = "startup.before_model_load";
inline constexpr const char* kHookEventAssistantMessageCompleted = "assistant.message_completed";

enum class HookMode {
    Sync = 0,
    Async = 1,
};

enum class HookPlatform {
    Windows = 0,
    Linux = 1,
    Mac = 2,
};

struct HookCommandSpec {
    std::string command;
    std::vector<std::string> args;

    bool valid() const { return !command.empty(); }
};

struct HookDefinition {
    std::string id;
    std::string event;
    HookMode mode = HookMode::Sync;
    int timeout_ms = 3000;
    std::vector<std::string> platforms;
    HookCommandSpec command;
    std::map<std::string, HookCommandSpec> commands_by_platform;
};

struct HookConfig {
    int version = 1;
    bool enabled = false;
    std::map<std::string, std::vector<HookDefinition>> events;
};

HookPlatform current_hook_platform();
std::string hook_platform_name(HookPlatform platform);
std::vector<std::string> hook_platform_command_resolution_order(HookPlatform platform);

std::string normalize_hook_platform_name(std::string value);
bool hook_platform_matches(const HookDefinition& hook, HookPlatform current);
std::optional<HookCommandSpec> resolve_hook_command(const HookDefinition& hook,
                                                    HookPlatform current);

HookConfig parse_hook_config_json(const nlohmann::json& j, std::string* error = nullptr);
HookConfig load_hook_config_from_path(const std::string& path, std::string* error = nullptr);
HookConfig load_hook_config(std::string* error = nullptr);
bool set_hook_config_enabled_in_path(const std::string& path,
                                     bool enabled,
                                     std::string* error = nullptr);

std::string default_hook_config_path();

} // namespace acecode
