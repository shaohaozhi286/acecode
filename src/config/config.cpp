#include "config.hpp"

#include "model_provider_registry.hpp"
#include "request_headers.hpp"
#include "../utils/constants.hpp"
#include "../utils/logger.hpp"
#include "../utils/paths.hpp"
#include "../utils/utf8_path.hpp"

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <set>

namespace fs = std::filesystem;

namespace acecode {

namespace {

std::atomic<bool> g_acecode_home_created_by_process{false};

std::string trim_ascii_copy(const std::string& s) {
    size_t first = 0;
    while (first < s.size() && std::isspace(static_cast<unsigned char>(s[first]))) {
        ++first;
    }
    size_t last = s.size();
    while (last > first && std::isspace(static_cast<unsigned char>(s[last - 1]))) {
        --last;
    }
    return s.substr(first, last - first);
}

bool is_one_of(const std::string& value, std::initializer_list<const char*> allowed) {
    for (const char* item : allowed) {
        if (value == item) return true;
    }
    return false;
}

std::string normalize_permission_mode_name(std::string value) {
    if (value == "acceptEdits") value = "accept-edits";
    if (is_one_of(value, {"default", "accept-edits", "plan", "yolo"})) {
        return value;
    }
    if (!value.empty()) {
        LOG_WARN("[config] default_permission_mode='" + value +
                 "' invalid; falling back to 'default'");
    }
    return "default";
}

std::optional<int> parse_positive_int(const std::string& value) {
    const std::string trimmed = trim_ascii_copy(value);
    if (trimmed.empty()) return std::nullopt;
    try {
        std::size_t pos = 0;
        long long parsed = std::stoll(trimmed, &pos, 10);
        if (pos != trimmed.size() ||
            parsed <= 0 ||
            parsed > std::numeric_limits<int>::max()) {
            return std::nullopt;
        }
        return static_cast<int>(parsed);
    } catch (...) {
        return std::nullopt;
    }
}

[[noreturn]] void fatal_config_value(const std::string& message) {
    std::cerr << "[config] fatal: " << message << std::endl;
    LOG_ERROR("[config] " + message);
    std::exit(1);
}

std::string legacy_model_profile_name(const AppConfig& cfg) {
    if (cfg.provider == "openai") {
        if (cfg.openai.models_dev_provider_id.has_value() &&
            !cfg.openai.models_dev_provider_id->empty()) {
            return *cfg.openai.models_dev_provider_id;
        }
        return "openai";
    }
    if (cfg.provider == "copilot") return "copilot";
    return "";
}

} // namespace

std::string normalize_upgrade_base_url(std::string raw) {
    raw = trim_ascii_copy(raw);
    if (!raw.empty() && raw.back() != '/') {
        raw.push_back('/');
    }
    return raw;
}

bool is_valid_upgrade_base_url(const std::string& raw) {
    const std::string url = normalize_upgrade_base_url(raw);
    return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
}

nlohmann::json connector_hook_to_json(const ConnectorHookConfig& hook) {
    return {
        {"command", hook.command},
        {"args", hook.args},
        {"timeout_ms", hook.timeout_ms},
    };
}

bool parse_connector_hook(const nlohmann::json& item,
                          ConnectorHookConfig& out,
                          std::string& error_msg) {
    if (!item.is_object() || !item.contains("command") || !item["command"].is_string()) {
        error_msg = "must be an object with string command";
        return false;
    }
    out.command = item["command"].get<std::string>();
    if (out.command.empty()) {
        error_msg = "command must not be empty";
        return false;
    }
    out.args.clear();
    if (item.contains("args")) {
        if (!item["args"].is_array()) {
            error_msg = "args must be an array of strings";
            return false;
        }
        for (const auto& arg : item["args"]) {
            if (!arg.is_string()) {
                error_msg = "args must be an array of strings";
                return false;
            }
            out.args.push_back(arg.get<std::string>());
        }
    }
    if (item.contains("timeout_ms")) {
        if (!item["timeout_ms"].is_number_integer()) {
            error_msg = "timeout_ms must be an integer";
            return false;
        }
        const int timeout = item["timeout_ms"].get<int>();
        if (timeout > 0) out.timeout_ms = timeout;
    }
    return true;
}

nlohmann::json connectors_to_json(const std::vector<ConnectorConfig>& connectors) {
    nlohmann::json items = nlohmann::json::array();
    for (const auto& connector : connectors) {
        nlohmann::json item = {
            {"id", connector.id},
            {"name", connector.name},
            {"description", connector.description},
            {"enabled", connector.enabled},
        };
        nlohmann::json hooks = nlohmann::json::object();
        if (connector.on_enable) {
            hooks["on_enable"] = connector_hook_to_json(*connector.on_enable);
        }
        if (connector.on_auth_error) {
            hooks["on_auth_error"] = connector_hook_to_json(*connector.on_auth_error);
        }
        if (!hooks.empty()) item["hooks"] = std::move(hooks);
        if (!connector.auth_error_base_url_prefix.empty()) {
            item["auth_error_scope"] = {
                {"base_url_prefix", connector.auth_error_base_url_prefix},
            };
        }
        items.push_back(std::move(item));
    }
    return items;
}

bool parse_connectors_json(const nlohmann::json& value,
                           std::vector<ConnectorConfig>& out,
                           std::string* error) {
    if (!value.is_array()) {
        if (error) *error = "connectors must be an array";
        return false;
    }

    std::vector<ConnectorConfig> parsed;
    std::set<std::string> seen_ids;
    parsed.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto& item = value[i];
        auto fail = [&](const std::string& message) {
            if (error) {
                *error = "connectors[" + std::to_string(i) + "] " + message;
            }
            return false;
        };
        if (!item.is_object()) return fail("must be an object");
        if (!item.contains("id") || !item["id"].is_string()) {
            return fail("must contain string id");
        }
        if (!item.contains("name") || !item["name"].is_string()) {
            return fail("must contain string name");
        }
        if (!item.contains("description") || !item["description"].is_string()) {
            return fail("must contain string description");
        }
        if (!item.contains("enabled") || !item["enabled"].is_boolean()) {
            return fail("must contain boolean enabled");
        }

        ConnectorConfig connector;
        connector.id = trim_ascii_copy(item["id"].get<std::string>());
        connector.name = item["name"].get<std::string>();
        connector.description = item["description"].get<std::string>();
        connector.enabled = item["enabled"].get<bool>();
        if (item.contains("hooks")) {
            const auto& hooks = item["hooks"];
            if (!hooks.is_object()) return fail("hooks must be an object");
            std::string hook_error;
            if (hooks.contains("on_enable")) {
                ConnectorHookConfig hook;
                if (!parse_connector_hook(hooks["on_enable"], hook, hook_error)) {
                    return fail("hooks.on_enable " + hook_error);
                }
                connector.on_enable = std::move(hook);
            }
            if (hooks.contains("on_auth_error")) {
                ConnectorHookConfig hook;
                if (!parse_connector_hook(hooks["on_auth_error"], hook, hook_error)) {
                    return fail("hooks.on_auth_error " + hook_error);
                }
                connector.on_auth_error = std::move(hook);
            }
        }
        if (item.contains("auth_error_scope")) {
            const auto& scope = item["auth_error_scope"];
            if (!scope.is_object()) return fail("auth_error_scope must be an object");
            if (scope.contains("base_url_prefix")) {
                if (!scope["base_url_prefix"].is_string()) {
                    return fail("auth_error_scope.base_url_prefix must be a string");
                }
                connector.auth_error_base_url_prefix =
                    trim_ascii_copy(scope["base_url_prefix"].get<std::string>());
            }
        }
        if (connector.id.empty()) return fail("id must not be empty");
        if (connector.name.empty()) return fail("name must not be empty");
        if (!seen_ids.insert(connector.id).second) {
            return fail("id must be unique: " + connector.id);
        }
        parsed.push_back(std::move(connector));
    }

    out = std::move(parsed);
    return true;
}

void load_connectors_lenient(const nlohmann::json& value,
                             std::vector<ConnectorConfig>& out) {
    if (!value.is_array()) {
        LOG_WARN("[config] 'connectors' must be an array, ignoring");
        return;
    }

    std::set<std::string> seen_ids;
    std::vector<ConnectorConfig> parsed;
    parsed.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto& item = value[i];
        if (!item.is_object() ||
            !item.contains("id") || !item["id"].is_string() ||
            !item.contains("name") || !item["name"].is_string() ||
            !item.contains("description") || !item["description"].is_string()) {
            LOG_WARN("[config] connectors[" + std::to_string(i) +
                     "] missing required id/name/description strings, skipping");
            continue;
        }

        ConnectorConfig connector;
        connector.id = trim_ascii_copy(item["id"].get<std::string>());
        connector.name = item["name"].get<std::string>();
        connector.description = item["description"].get<std::string>();
        connector.enabled = item.contains("enabled") && item["enabled"].is_boolean()
            ? item["enabled"].get<bool>()
            : true;
        if (connector.id.empty() || connector.name.empty()) {
            LOG_WARN("[config] connectors[" + std::to_string(i) +
                     "] has empty id or name, skipping");
            continue;
        }
        if (!seen_ids.insert(connector.id).second) {
            LOG_WARN("[config] duplicate connector id '" + connector.id + "', skipping");
            continue;
        }
        parsed.push_back(std::move(connector));
    }
    out = std::move(parsed);
}

ModelProfile legacy_model_profile_from_config(const AppConfig& cfg) {
    ModelProfile profile;
    profile.name = legacy_model_profile_name(cfg);
    if (cfg.provider == "openai") {
        OpenAiConfig defaults;
        profile.provider = "openai";
        profile.base_url = cfg.openai.base_url.empty()
            ? defaults.base_url
            : cfg.openai.base_url;
        profile.api_key = cfg.openai.api_key;
        profile.model = cfg.openai.model.empty()
            ? defaults.model
            : cfg.openai.model;
        profile.stream_timeout_ms = cfg.openai.stream_timeout_ms;
        profile.request_headers = cfg.openai.request_headers;
        profile.models_dev_provider_id = cfg.openai.models_dev_provider_id;
        return profile;
    }

    if (cfg.provider != "copilot") return profile;

    CopilotConfig defaults;
    profile.provider = "copilot";
    profile.model = cfg.copilot.model.empty()
        ? defaults.model
        : cfg.copilot.model;
    return profile;
}

std::string get_acecode_dir() {
    // 数据目录路径解析全部委托给 paths.cpp,RunMode 决定 User vs Service 根目录
    // (Decision 8)。User 模式行为与历史一致 — TUI / standalone daemon 不受影响。
    return resolve_data_dir(get_run_mode());
}

std::string get_run_dir() {
    // desktop 多 workspace 模式下 daemon 启动时会调 set_run_dir_override,
    // 把 run/ 切到 per-workspace 路径(避免共享 ~/.acecode/run/ 互相覆盖锁文件)。
    auto override_path = get_run_dir_override();
    if (!override_path.empty()) return override_path;
    return path_to_utf8(path_from_utf8(get_acecode_dir()) / constants::SUBDIR_RUN);
}

std::string get_logs_dir() {
    return path_to_utf8(path_from_utf8(get_acecode_dir()) / constants::SUBDIR_LOGS);
}

std::vector<std::string> validate_config(const AppConfig& cfg) {
    std::vector<std::string> errors;
    if (cfg.web.port < 1 || cfg.web.port > 65535) {
        errors.push_back("web.port out of range (1-65535): " + std::to_string(cfg.web.port));
    }
    if (cfg.web.bind.empty()) {
        errors.push_back("web.bind is empty; expected an IP address (e.g. 127.0.0.1)");
    }
    if (cfg.daemon.heartbeat_interval_ms <= 0) {
        errors.push_back("daemon.heartbeat_interval_ms must be > 0");
    }
    if (cfg.daemon.heartbeat_timeout_ms <= cfg.daemon.heartbeat_interval_ms) {
        errors.push_back("daemon.heartbeat_timeout_ms must be > daemon.heartbeat_interval_ms");
    }
    if (cfg.daemon.service_name.empty()) {
        errors.push_back("daemon.service_name is empty");
    }
    if (cfg.memory.max_index_bytes == 0) {
        errors.push_back("memory.max_index_bytes must be > 0");
    }
    if (cfg.openai.stream_timeout_ms <= 0) {
        errors.push_back("openai.stream_timeout_ms must be > 0");
    }
    if (cfg.session_title.max_input_bytes < 1 || cfg.session_title.max_input_bytes > 20000) {
        errors.push_back("session_title.max_input_bytes out of range (1-20000)");
    }
    if (cfg.session_title.timeout_ms < 1000 || cfg.session_title.timeout_ms > 120000) {
        errors.push_back("session_title.timeout_ms out of range (1000-120000)");
    }
    if (!cfg.openai.request_headers.empty()) {
        std::string err;
        if (!validate_request_headers(cfg.openai.request_headers, err)) {
            errors.push_back("openai." + err);
        }
    }
    if (cfg.project_instructions.max_depth < 1) {
        errors.push_back("project_instructions.max_depth must be >= 1");
    }
    if (cfg.project_instructions.max_bytes == 0) {
        errors.push_back("project_instructions.max_bytes must be > 0");
    }
    if (cfg.project_instructions.max_total_bytes < cfg.project_instructions.max_bytes) {
        errors.push_back("project_instructions.max_total_bytes must be >= max_bytes");
    }
    if (cfg.custom_instructions.text_snapshot().size() > kCustomInstructionsMaxBytes) {
        errors.push_back("custom_instructions.text exceeds " +
                         std::to_string(kCustomInstructionsMaxBytes) + " bytes");
    }
    std::set<std::string> connector_ids;
    for (const auto& connector : cfg.connectors) {
        if (trim_ascii_copy(connector.id).empty()) {
            errors.push_back("connectors.id must not be empty");
        } else if (!connector_ids.insert(trim_ascii_copy(connector.id)).second) {
            errors.push_back("connectors.id must be unique: " + connector.id);
        }
        if (connector.name.empty()) {
            errors.push_back("connectors.name must not be empty for id: " +
                             connector.id);
        }
    }
    if (!is_one_of(cfg.ace_browser_bridge.tool_mode, {"progressive", "compact", "full"})) {
        errors.push_back("ace_browser_bridge.tool_mode invalid: " +
                         cfg.ace_browser_bridge.tool_mode);
    }
    if (!is_one_of(cfg.ace_browser_bridge.default_mode, {"auto", "dom", "cdp", "os"})) {
        errors.push_back("ace_browser_bridge.default_mode invalid: " +
                         cfg.ace_browser_bridge.default_mode);
    }
    if (!is_one_of(cfg.ace_browser_bridge.pointer_speed, {"fast", "normal", "slow", "custom"})) {
        errors.push_back("ace_browser_bridge.pointer_speed invalid: " +
                         cfg.ace_browser_bridge.pointer_speed);
    }
    const auto& pc = cfg.ace_browser_bridge.pointer_custom;
    if (pc.move_duration_ms_min < 0 ||
        pc.move_duration_ms_max < pc.move_duration_ms_min ||
        pc.move_duration_ms_max > 10000) {
        errors.push_back("ace_browser_bridge.pointer_custom move duration out of range");
    }
    if (pc.click_hold_ms_min < 0 ||
        pc.click_hold_ms_max < pc.click_hold_ms_min ||
        pc.click_hold_ms_max > 5000) {
        errors.push_back("ace_browser_bridge.pointer_custom click hold out of range");
    }
    if (pc.typing_delay_ms_min < 0 ||
        pc.typing_delay_ms_max < pc.typing_delay_ms_min ||
        pc.typing_delay_ms_max > 5000) {
        errors.push_back("ace_browser_bridge.pointer_custom typing delay out of range");
    }
    if (pc.jitter_px < 0.0 || pc.jitter_px > 50.0) {
        errors.push_back("ace_browser_bridge.pointer_custom.jitter_px out of range");
    }
    if (pc.max_path_points < 2 || pc.max_path_points > 500) {
        errors.push_back("ace_browser_bridge.pointer_custom.max_path_points out of range");
    }
    if (cfg.ace_browser_bridge.status_cache_ttl_ms < 0 ||
        cfg.ace_browser_bridge.status_cache_ttl_ms > 10000) {
        errors.push_back("ace_browser_bridge.status_cache_ttl_ms out of range (0-10000): " +
                         std::to_string(cfg.ace_browser_bridge.status_cache_ttl_ms));
    }
    if (cfg.ace_browser_bridge.tool_timeout_ms < 1000 ||
        cfg.ace_browser_bridge.tool_timeout_ms > 300000) {
        errors.push_back("ace_browser_bridge.tool_timeout_ms out of range (1000-300000): " +
                         std::to_string(cfg.ace_browser_bridge.tool_timeout_ms));
    }
    if (cfg.ace_browser_bridge.operation_overlay_watchdog_ms < 1000 ||
        cfg.ace_browser_bridge.operation_overlay_watchdog_ms > 120000) {
        errors.push_back("ace_browser_bridge.operation_overlay_watchdog_ms out of range (1000-120000): " +
                         std::to_string(cfg.ace_browser_bridge.operation_overlay_watchdog_ms));
    }
    if (!is_valid_upgrade_base_url(cfg.upgrade.base_url)) {
        errors.push_back("upgrade.base_url must be a non-empty http or https URL");
    }
    if (cfg.upgrade.timeout_ms < 1000 || cfg.upgrade.timeout_ms > 120000) {
        errors.push_back("upgrade.timeout_ms out of range (1000-120000): " +
                         std::to_string(cfg.upgrade.timeout_ms));
    }
    if (cfg.remote_control.port < 1 || cfg.remote_control.port > 65535) {
        errors.push_back("remote_control.port out of range (1-65535): " +
                         std::to_string(cfg.remote_control.port));
    }
    if (!cfg.remote_control.default_channel.empty() &&
        cfg.remote_control.channels.find(cfg.remote_control.default_channel) ==
            cfg.remote_control.channels.end()) {
        errors.push_back("remote_control.default_channel references an undefined channel: " +
                         cfg.remote_control.default_channel);
    }
    for (const auto& [name, channel] : cfg.remote_control.channels) {
        if (name.empty()) {
            errors.push_back("remote_control.channels contains an empty channel name");
            continue;
        }
        bool bad_name = false;
        for (unsigned char ch : name) {
            if (std::isspace(ch) || ch == '/' || ch == '\\') {
                bad_name = true;
                break;
            }
        }
        if (bad_name) {
            errors.push_back("remote_control.channels." + name +
                             " must not contain whitespace or path separators");
        }
        if (channel.manifest_path.empty()) {
            errors.push_back("remote_control.channels." + name +
                             ".manifest_path must not be empty");
        }
        if (channel.timeout_ms < 1000 || channel.timeout_ms > 120000) {
            errors.push_back("remote_control.channels." + name +
                             ".timeout_ms out of range (1000-120000): " +
                             std::to_string(channel.timeout_ms));
        }
        if (!channel.settings.is_object()) {
            errors.push_back("remote_control.channels." + name +
                             ".settings must be a JSON object");
        }
    }
    for (const auto& fn : cfg.project_instructions.filenames) {
        if (fn.empty()) {
            errors.push_back("project_instructions.filenames contains empty entry");
            break;
        }
        if (fn.find('/') != std::string::npos || fn.find('\\') != std::string::npos) {
            errors.push_back("project_instructions.filenames entry must not contain path separator: " + fn);
            break;
        }
    }
    return errors;
}

static void write_default_config(const std::string& config_path) {
    nlohmann::json j;
    j["provider"] = "";
    j["openai"]["base_url"] = "http://localhost:1234/v1";
    j["openai"]["api_key"] = "";
    j["openai"]["model"] = "local-model";
    j["copilot"]["model"] = "gpt-4o";
    j["codex"]["model"] = "gpt-5.5";
    j["saved_models"] = nlohmann::json::array();
    j["default_model_name"] = "";
    j["default_permission_mode"] = "default";

    std::ofstream ofs(path_from_utf8(config_path));
    if (ofs.is_open()) {
        ofs << j.dump(2) << std::endl;
    }
}

static void synthesize_legacy_saved_model_if_needed(AppConfig& cfg,
                                                    bool saved_models_key_present) {
    if (!cfg.saved_models.empty()) return;
    if (saved_models_key_present) {
        if (!cfg.default_model_name.empty()) {
            LOG_WARN("[config] default_model_name ignored because saved_models is empty: " +
                     cfg.default_model_name);
            cfg.default_model_name.clear();
        }
        return;
    }

    ModelProfile legacy = legacy_model_profile_from_config(cfg);
    std::vector<ModelProfile> candidate{legacy};
    std::string err;
    if (validate_saved_models(candidate, legacy.name, err)) {
        cfg.saved_models = std::move(candidate);
        cfg.default_model_name = legacy.name;
        LOG_WARN("[config] saved_models missing; synthesized legacy model profile '" +
                 legacy.name + "' from provider/openai/copilot/codex fields");
        return;
    }

    if (!cfg.default_model_name.empty()) {
        LOG_WARN("[config] default_model_name ignored because saved_models is empty: " +
                 cfg.default_model_name);
        cfg.default_model_name.clear();
    }
    LOG_WARN("[config] saved_models missing and legacy fields cannot be migrated: " + err);
}

static const ModelProfile* find_profile_by_name(const std::vector<ModelProfile>& entries,
                                                const std::string& name) {
    if (name.empty()) return nullptr;
    for (const auto& entry : entries) {
        if (entry.name == name) return &entry;
    }
    return nullptr;
}

static const ModelProfile* first_enabled_profile(const std::vector<ModelProfile>& entries) {
    for (const auto& entry : entries) {
        if (is_runtime_model_provider_enabled(entry.provider)) return &entry;
    }
    return nullptr;
}

static void sanitize_disabled_model_providers(AppConfig& cfg) {
    bool provider_was_disabled = false;
    if (!cfg.provider.empty() && !is_runtime_model_provider_enabled(cfg.provider)) {
        LOG_WARN(std::string("[config] provider '") + cfg.provider +
                 "' is disabled; falling back to an enabled saved model");
        provider_was_disabled = true;
    }

    if (cfg.saved_models.empty()) {
        if (provider_was_disabled) cfg.provider.clear();
        return;
    }

    const ModelProfile* default_profile =
        find_profile_by_name(cfg.saved_models, cfg.default_model_name);
    if (default_profile &&
        is_runtime_model_provider_enabled(default_profile->provider)) {
        if (provider_was_disabled) cfg.provider = default_profile->provider;
        return;
    }

    if (default_profile) {
        LOG_WARN(std::string("[config] default model '") + cfg.default_model_name +
                 "' uses disabled provider '" + default_profile->provider + "'");
    }

    if (const ModelProfile* fallback = first_enabled_profile(cfg.saved_models)) {
        if (cfg.default_model_name != fallback->name) {
            LOG_WARN("[config] switching default model to enabled profile '" +
                     fallback->name + "'");
        }
        cfg.default_model_name = fallback->name;
        cfg.provider = fallback->provider;
        return;
    }

    LOG_WARN("[config] no enabled saved model profiles; clearing default model");
    cfg.default_model_name.clear();
    cfg.provider.clear();
}

AppConfig load_config() {
    AppConfig cfg;
    bool saved_models_key_present = false;

    std::string acecode_dir = get_acecode_dir();
    std::string config_path = path_to_utf8(path_from_utf8(acecode_dir) / "config.json");

    // Create directory and default config if missing
    std::error_code home_ec;
    fs::path native_acecode_dir = path_from_utf8(acecode_dir);
    fs::path native_config_path = path_from_utf8(config_path);
    bool home_exists = fs::exists(native_acecode_dir, home_ec);
    if (home_ec) home_exists = false;
    if (!home_exists) {
        fs::create_directories(native_acecode_dir);
        g_acecode_home_created_by_process.store(true);
    }
    if (!fs::exists(native_config_path)) {
        write_default_config(config_path);
    }

    // Read config file
    std::ifstream ifs(native_config_path);
    if (ifs.is_open()) {
        try {
            nlohmann::json j = nlohmann::json::parse(ifs);

            if (j.contains("provider") && j["provider"].is_string()) {
                cfg.provider = j["provider"].get<std::string>();
            }
            if (j.contains("openai") && j["openai"].is_object()) {
                auto& oj = j["openai"];
                if (oj.contains("base_url") && oj["base_url"].is_string())
                    cfg.openai.base_url = oj["base_url"].get<std::string>();
                if (oj.contains("api_key") && oj["api_key"].is_string())
                    cfg.openai.api_key = oj["api_key"].get<std::string>();
                if (oj.contains("model") && oj["model"].is_string())
                    cfg.openai.model = oj["model"].get<std::string>();
                if (oj.contains("stream_timeout_ms") &&
                    oj["stream_timeout_ms"].is_number_integer()) {
                    int v = oj["stream_timeout_ms"].get<int>();
                    if (v <= 0) {
                        fatal_config_value("openai.stream_timeout_ms=" +
                                           std::to_string(v) +
                                           " out of range (>0)");
                    }
                    cfg.openai.stream_timeout_ms = v;
                }
                if (oj.contains("models_dev_provider_id") &&
                    oj["models_dev_provider_id"].is_string()) {
                    cfg.openai.models_dev_provider_id =
                        oj["models_dev_provider_id"].get<std::string>();
                }
                if (oj.contains("request_headers")) {
                    std::string err;
                    auto parsed = parse_request_headers_json(
                        oj["request_headers"],
                        "openai",
                        err);
                    if (!parsed.has_value()) {
                        std::cerr << "[config] fatal: " << err << std::endl;
                        LOG_ERROR(std::string("[config] openai.request_headers parse failure: ") + err);
                        std::exit(1);
                    }
                    cfg.openai.request_headers = std::move(*parsed);
                }
            }
            if (j.contains("copilot") && j["copilot"].is_object()) {
                auto& cj = j["copilot"];
                if (cj.contains("model") && cj["model"].is_string())
                    cfg.copilot.model = cj["model"].get<std::string>();
            }
            if (j.contains("codex") && j["codex"].is_object()) {
                auto& cj = j["codex"];
                if (cj.contains("model") && cj["model"].is_string())
                    cfg.codex.model = cj["model"].get<std::string>();
            }
            if (j.contains("context_window") && j["context_window"].is_number_integer()) {
                cfg.context_window = j["context_window"].get<int>();
            }
            if (j.contains("max_sessions") && j["max_sessions"].is_number_integer()) {
                cfg.max_sessions = j["max_sessions"].get<int>();
            }
            if (j.contains("default_permission_mode") &&
                j["default_permission_mode"].is_string()) {
                cfg.default_permission_mode = normalize_permission_mode_name(
                    j["default_permission_mode"].get<std::string>());
            }
            if (j.contains("features") && j["features"].is_object()) {
                const auto& fj = j["features"];
                if (fj.contains("hooks") && fj["hooks"].is_boolean()) {
                    cfg.features.hooks = fj["hooks"].get<bool>();
                }
                if (fj.contains("completed_turn_self_heal") &&
                    fj["completed_turn_self_heal"].is_boolean()) {
                    cfg.features.completed_turn_self_heal =
                        fj["completed_turn_self_heal"].get<bool>();
                }
            }
            if (j.contains("skills") && j["skills"].is_object()) {
                const auto& sj = j["skills"];
                if (sj.contains("disabled") && sj["disabled"].is_array()) {
                    for (const auto& v : sj["disabled"]) {
                        if (v.is_string()) cfg.skills.disabled.push_back(v.get<std::string>());
                    }
                }
                if (sj.contains("external_dirs") && sj["external_dirs"].is_array()) {
                    for (const auto& v : sj["external_dirs"]) {
                        if (v.is_string()) cfg.skills.external_dirs.push_back(v.get<std::string>());
                    }
                }
                if (sj.contains("reuse_opencode") && sj["reuse_opencode"].is_boolean()) {
                    cfg.skills.reuse_opencode = sj["reuse_opencode"].get<bool>();
                }
            }
            if (j.contains("memory") && j["memory"].is_object()) {
                const auto& mj = j["memory"];
                if (mj.contains("enabled") && mj["enabled"].is_boolean())
                    cfg.memory.enabled = mj["enabled"].get<bool>();
                if (mj.contains("max_index_bytes") && mj["max_index_bytes"].is_number_integer()) {
                    long long v = mj["max_index_bytes"].get<long long>();
                    if (v > 0) cfg.memory.max_index_bytes = static_cast<std::size_t>(v);
                }
            }
            if (j.contains("project_instructions") && j["project_instructions"].is_object()) {
                const auto& pj = j["project_instructions"];
                if (pj.contains("enabled") && pj["enabled"].is_boolean())
                    cfg.project_instructions.enabled = pj["enabled"].get<bool>();
                if (pj.contains("max_depth") && pj["max_depth"].is_number_integer()) {
                    int v = pj["max_depth"].get<int>();
                    if (v > 0) cfg.project_instructions.max_depth = v;
                }
                if (pj.contains("max_bytes") && pj["max_bytes"].is_number_integer()) {
                    long long v = pj["max_bytes"].get<long long>();
                    if (v > 0) cfg.project_instructions.max_bytes = static_cast<std::size_t>(v);
                }
                if (pj.contains("max_total_bytes") && pj["max_total_bytes"].is_number_integer()) {
                    long long v = pj["max_total_bytes"].get<long long>();
                    if (v > 0) cfg.project_instructions.max_total_bytes = static_cast<std::size_t>(v);
                }
                if (pj.contains("filenames") && pj["filenames"].is_array()) {
                    std::vector<std::string> fns;
                    for (const auto& v : pj["filenames"]) {
                        if (v.is_string()) {
                            std::string s = v.get<std::string>();
                            if (!s.empty()) fns.push_back(std::move(s));
                        }
                    }
                    // Empty array -> keep the struct's default list so
                    // AGENT.md / CLAUDE.md still work out of the box.
                    if (!fns.empty()) cfg.project_instructions.filenames = std::move(fns);
                }
                if (pj.contains("read_claude_md") && pj["read_claude_md"].is_boolean())
                    cfg.project_instructions.read_claude_md = pj["read_claude_md"].get<bool>();
            }
            if (j.contains("custom_instructions") && j["custom_instructions"].is_object()) {
                const auto& cj = j["custom_instructions"];
                if (cj.contains("text") && cj["text"].is_string()) {
                    cfg.custom_instructions.set_text(cj["text"].get<std::string>());
                }
            }
            if (j.contains("connectors")) {
                load_connectors_lenient(j["connectors"], cfg.connectors);
            }
            if (j.contains("daemon") && j["daemon"].is_object()) {
                const auto& dj = j["daemon"];
                if (dj.contains("auto_start_on_double_click") && dj["auto_start_on_double_click"].is_boolean())
                    cfg.daemon.auto_start_on_double_click = dj["auto_start_on_double_click"].get<bool>();
                if (dj.contains("service_name") && dj["service_name"].is_string())
                    cfg.daemon.service_name = dj["service_name"].get<std::string>();
                if (dj.contains("heartbeat_interval_ms") && dj["heartbeat_interval_ms"].is_number_integer())
                    cfg.daemon.heartbeat_interval_ms = dj["heartbeat_interval_ms"].get<int>();
                if (dj.contains("heartbeat_timeout_ms") && dj["heartbeat_timeout_ms"].is_number_integer())
                    cfg.daemon.heartbeat_timeout_ms = dj["heartbeat_timeout_ms"].get<int>();
            }
            if (j.contains("web") && j["web"].is_object()) {
                const auto& wj = j["web"];
                if (wj.contains("enabled") && wj["enabled"].is_boolean())
                    cfg.web.enabled = wj["enabled"].get<bool>();
                if (wj.contains("bind") && wj["bind"].is_string())
                    cfg.web.bind = wj["bind"].get<std::string>();
                if (wj.contains("port") && wj["port"].is_number_integer())
                    cfg.web.port = wj["port"].get<int>();
                // static_dir is intentionally optional. null/missing -> embedded assets;
                // string -> filesystem path. Empty string is treated the same as null.
                if (wj.contains("static_dir") && wj["static_dir"].is_string())
                    cfg.web.static_dir = wj["static_dir"].get<std::string>();
            }
            if (j.contains("web_ui")) {
                if (!j["web_ui"].is_object()) {
                    LOG_WARN("[config] 'web_ui' must be an object, ignoring");
                }
            }
            if (j.contains("models_dev") && j["models_dev"].is_object()) {
                const auto& mj = j["models_dev"];
                if (mj.contains("allow_network") && mj["allow_network"].is_boolean())
                    cfg.models_dev.allow_network = mj["allow_network"].get<bool>();
                if (mj.contains("refresh_on_command_only") && mj["refresh_on_command_only"].is_boolean())
                    cfg.models_dev.refresh_on_command_only = mj["refresh_on_command_only"].get<bool>();
                if (mj.contains("user_override_path") && mj["user_override_path"].is_string()) {
                    std::string p = mj["user_override_path"].get<std::string>();
                    if (!p.empty()) cfg.models_dev.user_override_path = p;
                }
            }
            if (j.contains("input_history") && j["input_history"].is_object()) {
                const auto& ihj = j["input_history"];
                if (ihj.contains("enabled") && ihj["enabled"].is_boolean())
                    cfg.input_history.enabled = ihj["enabled"].get<bool>();
                if (ihj.contains("max_entries") && ihj["max_entries"].is_number_integer()) {
                    int v = ihj["max_entries"].get<int>();
                    if (v > 0) cfg.input_history.max_entries = v;
                }
            }
            if (j.contains("upgrade")) {
                if (!j["upgrade"].is_object()) {
                    LOG_WARN("[config] 'upgrade' must be an object, ignoring");
                } else {
                    const auto& uj = j["upgrade"];
                    if (uj.contains("base_url") && uj["base_url"].is_string()) {
                        cfg.upgrade.base_url =
                            normalize_upgrade_base_url(uj["base_url"].get<std::string>());
                    }
                    if (uj.contains("timeout_ms") && uj["timeout_ms"].is_number_integer()) {
                        cfg.upgrade.timeout_ms = uj["timeout_ms"].get<int>();
                    }
                }
            }
            if (j.contains("network") && j["network"].is_object()) {
                const auto& nj = j["network"];
                if (nj.contains("proxy_mode") && nj["proxy_mode"].is_string()) {
                    std::string m = nj["proxy_mode"].get<std::string>();
                    if (m != "auto" && m != "off" && m != "manual") {
                        std::cerr << "[config] fatal: network.proxy_mode='" << m
                                  << "' invalid; expected one of: auto, off, manual" << std::endl;
                        LOG_ERROR("[config] network.proxy_mode invalid: " + m);
                        std::exit(1);
                    }
                    cfg.network.proxy_mode = std::move(m);
                }
                if (nj.contains("proxy_url") && nj["proxy_url"].is_string())
                    cfg.network.proxy_url = nj["proxy_url"].get<std::string>();
                if (nj.contains("proxy_no_proxy") && nj["proxy_no_proxy"].is_string())
                    cfg.network.proxy_no_proxy = nj["proxy_no_proxy"].get<std::string>();
                if (nj.contains("proxy_probe_enabled") &&
                    nj["proxy_probe_enabled"].is_boolean())
                    cfg.network.proxy_probe_enabled =
                        nj["proxy_probe_enabled"].get<bool>();
                if (nj.contains("proxy_probe_timeout_ms") &&
                    nj["proxy_probe_timeout_ms"].is_number_integer()) {
                    int raw = nj["proxy_probe_timeout_ms"].get<int>();
                    int clamped = raw;
                    if (clamped < 200)   clamped = 200;
                    if (clamped > 10000) clamped = 10000;
                    if (clamped != raw) {
                        LOG_WARN("[config] network.proxy_probe_timeout_ms=" +
                                 std::to_string(raw) + " out of [200, 10000], clamped to " +
                                 std::to_string(clamped));
                    }
                    cfg.network.proxy_probe_timeout_ms = clamped;
                }

                if (cfg.network.proxy_mode == "manual" && cfg.network.proxy_url.empty()) {
                    std::cerr << "[config] fatal: network.proxy_mode='manual' requires "
                              << "non-empty network.proxy_url" << std::endl;
                    LOG_ERROR("[config] manual proxy_mode missing proxy_url");
                    std::exit(1);
                }
            }
            // 联网搜索段。缺省 → 默认值(enabled=true, backend=auto)。
            // 参见 openspec/changes/add-web-search-tool/specs/.../spec.md。
            if (j.contains("web_search") && j["web_search"].is_object()) {
                const auto& wsj = j["web_search"];
                if (wsj.contains("enabled") && wsj["enabled"].is_boolean())
                    cfg.web_search.enabled = wsj["enabled"].get<bool>();
                if (wsj.contains("backend") && wsj["backend"].is_string()) {
                    std::string b = wsj["backend"].get<std::string>();
                    if (b != "auto" && b != "duckduckgo" && b != "bing_cn" &&
                        b != "bochaai" && b != "tavily") {
                        std::cerr << "[config] fatal: web_search.backend='" << b
                                  << "' invalid; expected one of: auto, duckduckgo, "
                                  << "bing_cn, bochaai, tavily" << std::endl;
                        LOG_ERROR("[config] web_search.backend invalid: " + b);
                        std::exit(1);
                    }
                    cfg.web_search.backend = std::move(b);
                }
                if (wsj.contains("api_key") && wsj["api_key"].is_string())
                    cfg.web_search.api_key = wsj["api_key"].get<std::string>();
                if (wsj.contains("max_results") && wsj["max_results"].is_number_integer()) {
                    int v = wsj["max_results"].get<int>();
                    if (v < 1 || v > 10) {
                        std::cerr << "[config] fatal: web_search.max_results=" << v
                                  << " out of range (1..10)" << std::endl;
                        LOG_ERROR("[config] web_search.max_results out of range: " +
                                  std::to_string(v));
                        std::exit(1);
                    }
                    cfg.web_search.max_results = v;
                }
                if (wsj.contains("timeout_ms") && wsj["timeout_ms"].is_number_integer()) {
                    int v = wsj["timeout_ms"].get<int>();
                    if (v < 1000 || v > 30000) {
                        std::cerr << "[config] fatal: web_search.timeout_ms=" << v
                                  << " out of range (1000..30000)" << std::endl;
                        LOG_ERROR("[config] web_search.timeout_ms out of range: " +
                                  std::to_string(v));
                        std::exit(1);
                    }
                    cfg.web_search.timeout_ms = v;
                }
            }

            // LSP 段(openspec add-lsp-service)。缺省 → enabled=true、无覆盖。
            // 自定义 server(名字不在内置集合)的 command 校验推迟到注册表合并层
            // (config 层不感知内置名单,避免双份清单漂移)。
            if (j.contains("lsp") && j["lsp"].is_object()) {
                const auto& lspj = j["lsp"];
                if (lspj.contains("enabled") && lspj["enabled"].is_boolean())
                    cfg.lsp.enabled = lspj["enabled"].get<bool>();
                if (lspj.contains("servers") && lspj["servers"].is_object()) {
                    for (const auto& [name, sj] : lspj["servers"].items()) {
                        if (!sj.is_object()) {
                            std::cerr << "[config] fatal: lsp.servers." << name
                                      << " must be an object" << std::endl;
                            LOG_ERROR("[config] lsp.servers entry not object: " + name);
                            std::exit(1);
                        }
                        LspServerOverride entry;
                        if (sj.contains("disabled") && sj["disabled"].is_boolean())
                            entry.disabled = sj["disabled"].get<bool>();
                        if (sj.contains("command")) {
                            if (!sj["command"].is_array()) {
                                std::cerr << "[config] fatal: lsp.servers." << name
                                          << ".command must be a string array" << std::endl;
                                LOG_ERROR("[config] lsp command not array: " + name);
                                std::exit(1);
                            }
                            for (const auto& item : sj["command"]) {
                                if (!item.is_string()) {
                                    std::cerr << "[config] fatal: lsp.servers." << name
                                              << ".command items must be strings" << std::endl;
                                    LOG_ERROR("[config] lsp command item not string: " + name);
                                    std::exit(1);
                                }
                                entry.command.push_back(item.get<std::string>());
                            }
                        }
                        if (sj.contains("extensions") && sj["extensions"].is_array()) {
                            for (const auto& item : sj["extensions"]) {
                                if (item.is_string())
                                    entry.extensions.push_back(item.get<std::string>());
                            }
                        }
                        if (sj.contains("env") && sj["env"].is_object()) {
                            for (const auto& [k, v] : sj["env"].items()) {
                                if (v.is_string()) entry.env[k] = v.get<std::string>();
                            }
                        }
                        if (sj.contains("initialization") && sj["initialization"].is_object())
                            entry.initialization = sj["initialization"];
                        cfg.lsp.servers[name] = std::move(entry);
                    }
                }
            }

            // Worktree 段。缺省 → 默认值(不 symlink、完整 checkout)。
            // 非法条目(非字符串)静默跳过,不阻塞启动。
            if (j.contains("worktree") && j["worktree"].is_object()) {
                const auto& wtj = j["worktree"];
                if (wtj.contains("symlink_directories") &&
                    wtj["symlink_directories"].is_array()) {
                    for (const auto& item : wtj["symlink_directories"]) {
                        if (item.is_string()) {
                            cfg.worktree.symlink_directories.push_back(
                                item.get<std::string>());
                        }
                    }
                }
                if (wtj.contains("sparse_paths") && wtj["sparse_paths"].is_array()) {
                    for (const auto& item : wtj["sparse_paths"]) {
                        if (item.is_string()) {
                            cfg.worktree.sparse_paths.push_back(item.get<std::string>());
                        }
                    }
                }
            }

            // git 感知段(openspec add-git-context)。缺省 → enabled=true、
            // timeout 3000ms。timeout 越界不 fatal,静默 clamp —— 该值只影响
            // best-effort 的采集行为,不值得阻塞启动。
            if (j.contains("git_context") && j["git_context"].is_object()) {
                const auto& gcj = j["git_context"];
                if (gcj.contains("enabled") && gcj["enabled"].is_boolean())
                    cfg.git_context.enabled = gcj["enabled"].get<bool>();
                if (gcj.contains("timeout_ms") &&
                    gcj["timeout_ms"].is_number_integer()) {
                    int v = gcj["timeout_ms"].get<int>();
                    if (v < 500) v = 500;
                    if (v > 30000) v = 30000;
                    cfg.git_context.timeout_ms = v;
                }
            }

            if (j.contains("remote_control") && j["remote_control"].is_object()) {
                const auto& rcj = j["remote_control"];
                if (rcj.contains("port") && rcj["port"].is_number_integer()) {
                    int v = rcj["port"].get<int>();
                    if (v < 1 || v > 65535) {
                        std::cerr << "[config] fatal: remote_control.port=" << v
                                  << " out of range (1..65535)" << std::endl;
                        LOG_ERROR("[config] remote_control.port out of range: " +
                                  std::to_string(v));
                        std::exit(1);
                    }
                    cfg.remote_control.port = v;
                }
                if (rcj.contains("token") && rcj["token"].is_string())
                    cfg.remote_control.token = rcj["token"].get<std::string>();
                if (rcj.contains("outbound_url") && rcj["outbound_url"].is_string())
                    cfg.remote_control.outbound_url = rcj["outbound_url"].get<std::string>();
                if (rcj.contains("default_channel") && rcj["default_channel"].is_string())
                    cfg.remote_control.default_channel =
                        rcj["default_channel"].get<std::string>();
                if (rcj.contains("channels") && rcj["channels"].is_object()) {
                    for (const auto& item : rcj["channels"].items()) {
                        if (!item.value().is_object()) continue;
                        RemoteControlConfig::ChannelPluginConfig channel;
                        const auto& cj = item.value();
                        if (cj.contains("manifest_path") && cj["manifest_path"].is_string())
                            channel.manifest_path = cj["manifest_path"].get<std::string>();
                        if (cj.contains("timeout_ms") && cj["timeout_ms"].is_number_integer())
                            channel.timeout_ms = cj["timeout_ms"].get<int>();
                        if (cj.contains("settings"))
                            channel.settings = cj["settings"];
                        cfg.remote_control.channels[item.key()] = std::move(channel);
                    }
                }
            }

            // Browser bridge tools. Canonical config key is ace_browser_bridge;
            // accept ace-browser-bridge as a compatibility alias for docs/tools.
            const nlohmann::json* abj_ptr = nullptr;
            if (j.contains("ace_browser_bridge")) {
                abj_ptr = &j["ace_browser_bridge"];
            } else if (j.contains("ace-browser-bridge")) {
                abj_ptr = &j["ace-browser-bridge"];
            }
            if (abj_ptr != nullptr) {
                const auto& abj = *abj_ptr;
                if (!abj.is_object()) {
                    LOG_WARN("[config] 'ace_browser_bridge' must be an object, ignoring");
                } else {
                    if (abj.contains("enabled") && abj["enabled"].is_boolean())
                        cfg.ace_browser_bridge.enabled = abj["enabled"].get<bool>();
                    if (abj.contains("host_path") && abj["host_path"].is_string()) {
                        cfg.ace_browser_bridge.host_path = abj["host_path"].get<std::string>();
                    } else if (abj.contains("cli_path") && abj["cli_path"].is_string()) {
                        cfg.ace_browser_bridge.host_path = abj["cli_path"].get<std::string>();
                    }
                    if (abj.contains("tool_mode") && abj["tool_mode"].is_string()) {
                        std::string v = abj["tool_mode"].get<std::string>();
                        if (!is_one_of(v, {"progressive", "compact", "full"})) {
                            fatal_config_value("ace_browser_bridge.tool_mode='" + v +
                                               "' invalid; expected one of: progressive, compact, full");
                        }
                        cfg.ace_browser_bridge.tool_mode = std::move(v);
                    }
                    if (abj.contains("default_mode") && abj["default_mode"].is_string()) {
                        std::string v = abj["default_mode"].get<std::string>();
                        if (!is_one_of(v, {"auto", "dom", "cdp", "os"})) {
                            fatal_config_value("ace_browser_bridge.default_mode='" + v +
                                               "' invalid; expected one of: auto, dom, cdp, os");
                        }
                        cfg.ace_browser_bridge.default_mode = std::move(v);
                    }
                    if (abj.contains("pointer_speed") && abj["pointer_speed"].is_string()) {
                        std::string v = abj["pointer_speed"].get<std::string>();
                        if (!is_one_of(v, {"fast", "normal", "slow", "custom"})) {
                            fatal_config_value("ace_browser_bridge.pointer_speed='" + v +
                                               "' invalid; expected one of: fast, normal, slow, custom");
                        }
                        cfg.ace_browser_bridge.pointer_speed = std::move(v);
                    }
                    if (abj.contains("status_cache_ttl_ms") &&
                        abj["status_cache_ttl_ms"].is_number_integer()) {
                        int v = abj["status_cache_ttl_ms"].get<int>();
                        if (v < 0 || v > 10000) {
                            fatal_config_value("ace_browser_bridge.status_cache_ttl_ms=" +
                                               std::to_string(v) + " out of range (0..10000)");
                        }
                        cfg.ace_browser_bridge.status_cache_ttl_ms = v;
                    }
                    if (abj.contains("tool_timeout_ms") &&
                        abj["tool_timeout_ms"].is_number_integer()) {
                        int v = abj["tool_timeout_ms"].get<int>();
                        if (v < 1000 || v > 300000) {
                            fatal_config_value("ace_browser_bridge.tool_timeout_ms=" +
                                               std::to_string(v) + " out of range (1000..300000)");
                        }
                        cfg.ace_browser_bridge.tool_timeout_ms = v;
                    }
                    if (abj.contains("os_pointer_enabled") &&
                        abj["os_pointer_enabled"].is_boolean())
                        cfg.ace_browser_bridge.os_pointer_enabled =
                            abj["os_pointer_enabled"].get<bool>();
                    if (abj.contains("tab_group_enabled") &&
                        abj["tab_group_enabled"].is_boolean())
                        cfg.ace_browser_bridge.tab_group_enabled =
                            abj["tab_group_enabled"].get<bool>();
                    if (abj.contains("operation_overlay_enabled") &&
                        abj["operation_overlay_enabled"].is_boolean())
                        cfg.ace_browser_bridge.operation_overlay_enabled =
                            abj["operation_overlay_enabled"].get<bool>();
                    if (abj.contains("operation_overlay_watchdog_ms") &&
                        abj["operation_overlay_watchdog_ms"].is_number_integer()) {
                        int v = abj["operation_overlay_watchdog_ms"].get<int>();
                        if (v < 1000 || v > 120000) {
                            fatal_config_value("ace_browser_bridge.operation_overlay_watchdog_ms=" +
                                               std::to_string(v) + " out of range (1000..120000)");
                        }
                        cfg.ace_browser_bridge.operation_overlay_watchdog_ms = v;
                    }
                    if (abj.contains("pointer_custom")) {
                        if (!abj["pointer_custom"].is_object()) {
                            LOG_WARN("[config] 'ace_browser_bridge.pointer_custom' must be an object, ignoring");
                        } else {
                            const auto& pcj = abj["pointer_custom"];
                            auto parse_int_range = [&](const char* key, int min_v, int max_v, int& out) {
                                if (!pcj.contains(key) || !pcj[key].is_number_integer()) return;
                                int v = pcj[key].get<int>();
                                if (v < min_v || v > max_v) {
                                    fatal_config_value(std::string("ace_browser_bridge.pointer_custom.") +
                                                       key + "=" + std::to_string(v) +
                                                       " out of range (" + std::to_string(min_v) +
                                                       ".." + std::to_string(max_v) + ")");
                                }
                                out = v;
                            };
                            parse_int_range("move_duration_ms_min", 0, 10000,
                                            cfg.ace_browser_bridge.pointer_custom.move_duration_ms_min);
                            parse_int_range("move_duration_ms_max", 0, 10000,
                                            cfg.ace_browser_bridge.pointer_custom.move_duration_ms_max);
                            parse_int_range("click_hold_ms_min", 0, 5000,
                                            cfg.ace_browser_bridge.pointer_custom.click_hold_ms_min);
                            parse_int_range("click_hold_ms_max", 0, 5000,
                                            cfg.ace_browser_bridge.pointer_custom.click_hold_ms_max);
                            parse_int_range("typing_delay_ms_min", 0, 5000,
                                            cfg.ace_browser_bridge.pointer_custom.typing_delay_ms_min);
                            parse_int_range("typing_delay_ms_max", 0, 5000,
                                            cfg.ace_browser_bridge.pointer_custom.typing_delay_ms_max);
                            parse_int_range("max_path_points", 2, 500,
                                            cfg.ace_browser_bridge.pointer_custom.max_path_points);
                            if (pcj.contains("jitter_px") && pcj["jitter_px"].is_number()) {
                                double v = pcj["jitter_px"].get<double>();
                                if (v < 0.0 || v > 50.0) {
                                    fatal_config_value("ace_browser_bridge.pointer_custom.jitter_px=" +
                                                       std::to_string(v) +
                                                       " out of range (0..50)");
                                }
                                cfg.ace_browser_bridge.pointer_custom.jitter_px = v;
                            }
                            const auto& pc = cfg.ace_browser_bridge.pointer_custom;
                            if (pc.move_duration_ms_max < pc.move_duration_ms_min) {
                                fatal_config_value("ace_browser_bridge.pointer_custom move_duration_ms_max "
                                                   "must be >= move_duration_ms_min");
                            }
                            if (pc.click_hold_ms_max < pc.click_hold_ms_min) {
                                fatal_config_value("ace_browser_bridge.pointer_custom click_hold_ms_max "
                                                   "must be >= click_hold_ms_min");
                            }
                            if (pc.typing_delay_ms_max < pc.typing_delay_ms_min) {
                                fatal_config_value("ace_browser_bridge.pointer_custom typing_delay_ms_max "
                                                   "must be >= typing_delay_ms_min");
                            }
                        }
                    }
                }
            }

            // TUI 渲染策略段。不存在时保持 TuiConfig 默认值(alt_screen_mode="auto")。
            // 非对象类型 + 非法字符串值都规范化到 "auto",启动不阻断。
            if (j.contains("tui")) {
                if (!j["tui"].is_object()) {
                    LOG_WARN("[config] 'tui' must be an object, ignoring");
                } else {
                    const auto& tj = j["tui"];
                    if (tj.contains("alt_screen_mode") && tj["alt_screen_mode"].is_string()) {
                        std::string m = tj["alt_screen_mode"].get<std::string>();
                        if (m == "auto" || m == "always" || m == "never") {
                            cfg.tui.alt_screen_mode = std::move(m);
                        } else {
                            LOG_WARN("[config] invalid tui.alt_screen_mode value '" + m +
                                     "', falling back to 'auto'");
                            cfg.tui.alt_screen_mode = "auto";
                        }
                    }
                    if (tj.contains("page_keys_single_line") &&
                        tj["page_keys_single_line"].is_boolean()) {
                        cfg.tui.page_keys_single_line =
                            tj["page_keys_single_line"].get<bool>();
                    }
                    if (tj.contains("theme") && tj["theme"].is_string()) {
                        std::string t = tj["theme"].get<std::string>();
                        if (t == "auto" || t == "dark" || t == "light") {
                            cfg.tui.theme = std::move(t);
                        } else {
                            LOG_WARN("[config] invalid tui.theme value '" + t +
                                     "', falling back to 'auto'");
                            cfg.tui.theme = "auto";
                        }
                    }
                }
            }

            // Desktop shell 配置 — 目前只挂 notifications。
            // 字段缺失 / 类型错都走默认值(全 true),启动不阻断。
            if (j.contains("desktop")) {
                if (!j["desktop"].is_object()) {
                    LOG_WARN("[config] 'desktop' must be an object, ignoring");
                } else {
                    const auto& dj = j["desktop"];
                    if (dj.contains("close_to_tray") && dj["close_to_tray"].is_boolean()) {
                        cfg.desktop.close_to_tray = dj["close_to_tray"].get<bool>();
                    }
                    if (dj.contains("notifications")) {
                        if (!dj["notifications"].is_object()) {
                            LOG_WARN("[config] 'desktop.notifications' must be an object, "
                                     "using defaults");
                        } else {
                            const auto& nj = dj["notifications"];
                            if (nj.contains("enabled") && nj["enabled"].is_boolean()) {
                                cfg.desktop.notifications.enabled = nj["enabled"].get<bool>();
                            }
                            if (nj.contains("on_question") && nj["on_question"].is_boolean()) {
                                cfg.desktop.notifications.on_question = nj["on_question"].get<bool>();
                            }
                            if (nj.contains("on_completion") && nj["on_completion"].is_boolean()) {
                                cfg.desktop.notifications.on_completion = nj["on_completion"].get<bool>();
                            }
                            if (nj.contains("suppress_when_focused") &&
                                nj["suppress_when_focused"].is_boolean()) {
                                cfg.desktop.notifications.suppress_when_focused =
                                    nj["suppress_when_focused"].get<bool>();
                            }
                        }
                    }
                }
            }

            // Web 控制台(add-console-dock):shell 覆盖 + + 旁下拉选择器
            // (default_shell / git_bash_path,见 控制台 Shell 选择器 plan)。
            if (j.contains("console")) {
                if (!j["console"].is_object()) {
                    LOG_WARN("[config] 'console' must be an object, ignoring");
                } else {
                    const auto& cj = j["console"];
                    if (cj.contains("shell") && cj["shell"].is_string()) {
                        cfg.console.shell = cj["shell"].get<std::string>();
                    }
                    if (cj.contains("default_shell") && cj["default_shell"].is_string()) {
                        cfg.console.default_shell = cj["default_shell"].get<std::string>();
                    }
                    if (cj.contains("git_bash_path") && cj["git_bash_path"].is_string()) {
                        cfg.console.git_bash_path = cj["git_bash_path"].get<std::string>();
                    }
                }
            }

            if (j.contains("session_title")) {
                if (!j["session_title"].is_object()) {
                    LOG_WARN("[config] 'session_title' must be an object, ignoring");
                } else {
                    const auto& stj = j["session_title"];
                    if (stj.contains("enabled") && stj["enabled"].is_boolean()) {
                        cfg.session_title.enabled = stj["enabled"].get<bool>();
                    }
                    if (stj.contains("model_name") && stj["model_name"].is_string()) {
                        cfg.session_title.model_name = stj["model_name"].get<std::string>();
                    }
                    if (stj.contains("max_input_bytes") &&
                        stj["max_input_bytes"].is_number_integer()) {
                        cfg.session_title.max_input_bytes = stj["max_input_bytes"].get<int>();
                    }
                    if (stj.contains("timeout_ms") && stj["timeout_ms"].is_number_integer()) {
                        cfg.session_title.timeout_ms = stj["timeout_ms"].get<int>();
                    }
                }
            }

            if (j.contains("agent_loop") && j["agent_loop"].is_object()) {
                const auto& alj = j["agent_loop"];
                // max_iterations = 0 disables the cap. Positive values are
                // clamped to [1, 10000].
                if (alj.contains("max_iterations") && alj["max_iterations"].is_number_integer()) {
                    int v = alj["max_iterations"].get<int>();
                    if (v < 0) {
                        LOG_WARN("[config] agent_loop.max_iterations=" + std::to_string(v) +
                                 " is out of range (min 0); clamping to 0");
                        v = 0;
                    } else if (v > 10000) {
                        LOG_WARN("[config] agent_loop.max_iterations=" + std::to_string(v) +
                                 " is out of range (max 10000); clamping to 10000");
                        v = 10000;
                    }
                    cfg.agent_loop.max_iterations = v;
                }
                // AskUserQuestion 应答策略(add-ask-question-policy)。显式含键
                // = 置 explicit 标记(即使值等于默认 "ask"),用于压制 YOLO 隐式
                // 映射;非法值归一化为 "ask" 且不置标记。
                if (alj.contains("question_policy") && alj["question_policy"].is_string()) {
                    std::string qp = alj["question_policy"].get<std::string>();
                    if (qp == "ask" || qp == "deny" || qp == "timeout") {
                        cfg.agent_loop.question_policy = qp;
                        cfg.agent_loop.question_policy_explicit = true;
                    } else {
                        LOG_WARN("[config] agent_loop.question_policy=\"" + qp +
                                 "\" is invalid (expected ask|deny|timeout); using \"ask\"");
                        cfg.agent_loop.question_policy = "ask";
                    }
                }
                if (alj.contains("question_timeout_seconds") &&
                    alj["question_timeout_seconds"].is_number_integer()) {
                    int v = alj["question_timeout_seconds"].get<int>();
                    if (v < 5 || v > 3600) {
                        LOG_WARN("[config] agent_loop.question_timeout_seconds=" +
                                 std::to_string(v) +
                                 " is out of range [5, 3600]; using default 60");
                        v = 60;
                    }
                    cfg.agent_loop.question_timeout_seconds = v;
                }
                // Legacy keys (auto_continue, max_consecutive_empty_iterations)
                // from the just-rolled-back agentic-loop-terminator change are
                // silently ignored — see align-loop-with-hermes.
            }
            // --- model profiles (openspec/changes/model-profiles) ---
            // 缺失视为旧 schema,load_config 末尾会从 legacy provider 字段合成兜底。
            if (j.contains("saved_models")) {
                saved_models_key_present = true;
                std::string err;
                auto parsed = parse_saved_models(j["saved_models"], err);
                if (!parsed.has_value()) {
                    std::cerr << "[config] fatal: " << err << std::endl;
                    LOG_ERROR(std::string("[config] saved_models parse failure: ") + err);
                    std::exit(1);
                }
                cfg.saved_models = std::move(*parsed);
            }
            if (j.contains("default_model_name") && j["default_model_name"].is_string()) {
                cfg.default_model_name = j["default_model_name"].get<std::string>();
            }

            if (j.contains("mcp_servers") && j["mcp_servers"].is_object()) {
                for (auto it = j["mcp_servers"].begin(); it != j["mcp_servers"].end(); ++it) {
                    const std::string& server_name = it.key();
                    const auto& sj = it.value();
                    if (!sj.is_object()) {
                        LOG_WARN("[config] mcp_servers['" + server_name + "'] is not an object, skipping");
                        continue;
                    }

                    McpServerConfig mcfg;

                    // 设置页开关持久化字段:true = 全 app 禁用。缺省视为启用。
                    if (sj.contains("disabled") && sj["disabled"].is_boolean()) {
                        mcfg.disabled = sj["disabled"].get<bool>();
                    }

                    // Determine transport. Missing field defaults to stdio so
                    // pre-existing configs keep working unchanged.
                    std::string transport_str = "stdio";
                    if (sj.contains("transport") && sj["transport"].is_string()) {
                        transport_str = sj["transport"].get<std::string>();
                    }
                    if (transport_str == "stdio") {
                        mcfg.transport = McpTransport::Stdio;
                    } else if (transport_str == "sse") {
                        mcfg.transport = McpTransport::Sse;
                    } else if (transport_str == "http") {
                        mcfg.transport = McpTransport::Http;
                    } else {
                        LOG_WARN("[config] mcp_servers['" + server_name +
                                 "'] has unknown transport '" + transport_str + "', skipping");
                        continue;
                    }

                    if (mcfg.transport == McpTransport::Stdio) {
                        if (!sj.contains("command") || !sj["command"].is_string() ||
                            sj["command"].get<std::string>().empty()) {
                            LOG_WARN("[config] mcp_servers['" + server_name +
                                     "'] stdio entry missing required 'command', skipping");
                            continue;
                        }
                        mcfg.command = sj["command"].get<std::string>();
                        if (sj.contains("args") && sj["args"].is_array()) {
                            for (const auto& a : sj["args"]) {
                                if (a.is_string()) mcfg.args.push_back(a.get<std::string>());
                            }
                        }
                        if (sj.contains("env") && sj["env"].is_object()) {
                            for (auto eit = sj["env"].begin(); eit != sj["env"].end(); ++eit) {
                                if (eit.value().is_string()) {
                                    mcfg.env[eit.key()] = eit.value().get<std::string>();
                                }
                            }
                        }
                    } else {
                        if (!sj.contains("url") || !sj["url"].is_string() ||
                            sj["url"].get<std::string>().empty()) {
                            LOG_WARN("[config] mcp_servers['" + server_name +
                                     "'] " + transport_str + " entry missing required 'url', skipping");
                            continue;
                        }
                        mcfg.url = sj["url"].get<std::string>();
                        if (sj.contains("sse_endpoint") && sj["sse_endpoint"].is_string()) {
                            mcfg.sse_endpoint = sj["sse_endpoint"].get<std::string>();
                        }
                        if (sj.contains("headers") && sj["headers"].is_object()) {
                            for (auto hit = sj["headers"].begin(); hit != sj["headers"].end(); ++hit) {
                                if (hit.value().is_string()) {
                                    mcfg.headers[hit.key()] = hit.value().get<std::string>();
                                }
                            }
                        }
                        if (sj.contains("auth_token") && sj["auth_token"].is_string()) {
                            mcfg.auth_token = sj["auth_token"].get<std::string>();
                        }
                        if (sj.contains("timeout_seconds") && sj["timeout_seconds"].is_number_integer()) {
                            int t = sj["timeout_seconds"].get<int>();
                            if (t > 0) mcfg.timeout_seconds = t;
                        }
                    }

                    cfg.mcp_servers[server_name] = std::move(mcfg);
                }
            }
        } catch (const nlohmann::json::parse_error& e) {
            std::cerr << "[config] Warning: Failed to parse config.json: " << e.what() << std::endl;
        }
    }

    // Environment variable overrides
    std::string env;
    if (getenv_utf8("ACECODE_PROVIDER", env)) {
        cfg.provider = env;
    }
    if (getenv_utf8("ACECODE_OPENAI_BASE_URL", env)) {
        cfg.openai.base_url = env;
    }
    if (getenv_utf8("ACECODE_OPENAI_API_KEY", env)) {
        cfg.openai.api_key = env;
    }
    if (getenv_utf8("ACECODE_OPENAI_STREAM_TIMEOUT_MS", env)) {
        auto parsed = parse_positive_int(env);
        if (parsed.has_value()) {
            cfg.openai.stream_timeout_ms = *parsed;
        } else {
            LOG_WARN("[config] ACECODE_OPENAI_STREAM_TIMEOUT_MS='" + env +
                     "' invalid; expected positive integer, keeping " +
                     std::to_string(cfg.openai.stream_timeout_ms));
        }
    }
    if (getenv_utf8("ACECODE_MODEL", env)) {
        if (cfg.provider == "openai") {
            cfg.openai.model = env;
        } else if (cfg.provider == "codex") {
            cfg.codex.model = env;
        } else {
            cfg.copilot.model = env;
        }
    }
    if (getenv_utf8("ACECODE_UPGRADE_BASE_URL", env)) {
        cfg.upgrade.base_url = normalize_upgrade_base_url(env);
    }

    synthesize_legacy_saved_model_if_needed(cfg, saved_models_key_present);
    if (!cfg.saved_models.empty()) {
        std::string err;
        if (!validate_saved_models(cfg.saved_models, cfg.default_model_name, err)) {
            std::cerr << "[config] fatal: " << err << std::endl;
            LOG_ERROR(std::string("[config] saved_models validation failure: ") + err);
            std::exit(1);
        }
        sanitize_disabled_model_providers(cfg);
    } else if (!cfg.default_model_name.empty()) {
        LOG_WARN("[config] default_model_name ignored because saved_models is empty: " +
                 cfg.default_model_name);
        cfg.default_model_name.clear();
    }
    if (cfg.saved_models.empty() &&
        !cfg.provider.empty() &&
        !is_runtime_model_provider_enabled(cfg.provider)) {
        LOG_WARN("[config] provider '" + cfg.provider +
                 "' ignored because no enabled model profiles are configured");
        cfg.provider.clear();
    }

    return cfg;
}

bool was_acecode_home_created_by_process() {
    return g_acecode_home_created_by_process.load();
}

bool consume_acecode_home_created_by_process() {
    return g_acecode_home_created_by_process.exchange(false);
}

void reset_acecode_home_created_flag_for_test() {
    g_acecode_home_created_by_process.store(false);
}

namespace {

nlohmann::json build_config_json(const AppConfig& cfg) {
    nlohmann::json j;
    j["provider"] = cfg.provider;
    j["openai"]["base_url"] = cfg.openai.base_url;
    j["openai"]["api_key"] = cfg.openai.api_key;
    j["openai"]["model"] = cfg.openai.model;
    if (cfg.openai.stream_timeout_ms != OpenAiConfig::kDefaultStreamTimeoutMs) {
        j["openai"]["stream_timeout_ms"] = cfg.openai.stream_timeout_ms;
    }
    if (cfg.openai.models_dev_provider_id.has_value() &&
        !cfg.openai.models_dev_provider_id->empty()) {
        j["openai"]["models_dev_provider_id"] = *cfg.openai.models_dev_provider_id;
    }
    if (!cfg.openai.request_headers.empty()) {
        nlohmann::json headers = nlohmann::json::object();
        for (const auto& [k, v] : cfg.openai.request_headers) headers[k] = v;
        j["openai"]["request_headers"] = std::move(headers);
    }
    j["copilot"]["model"] = cfg.copilot.model;
    j["codex"]["model"] = cfg.codex.model;
    j["context_window"] = cfg.context_window;
    j["max_sessions"] = cfg.max_sessions;
    if (normalize_permission_mode_name(cfg.default_permission_mode) != "default") {
        j["default_permission_mode"] =
            normalize_permission_mode_name(cfg.default_permission_mode);
    }

    SkillsConfig skills_d;
    if (!cfg.skills.disabled.empty() ||
        !cfg.skills.external_dirs.empty() ||
        cfg.skills.reuse_opencode != skills_d.reuse_opencode) {
        nlohmann::json sj = nlohmann::json::object();
        if (!cfg.skills.disabled.empty()) sj["disabled"] = cfg.skills.disabled;
        if (!cfg.skills.external_dirs.empty()) sj["external_dirs"] = cfg.skills.external_dirs;
        if (cfg.skills.reuse_opencode != skills_d.reuse_opencode)
            sj["reuse_opencode"] = cfg.skills.reuse_opencode;
        j["skills"] = sj;
    }

    {
        // Persist daemon and web sections so users discover the available
        // tunables; emit only fields that diverge from defaults to keep
        // the file readable.
        DaemonConfig dd;
        nlohmann::json dj = nlohmann::json::object();
        if (cfg.daemon.auto_start_on_double_click != dd.auto_start_on_double_click)
            dj["auto_start_on_double_click"] = cfg.daemon.auto_start_on_double_click;
        if (cfg.daemon.service_name != dd.service_name)
            dj["service_name"] = cfg.daemon.service_name;
        if (cfg.daemon.heartbeat_interval_ms != dd.heartbeat_interval_ms)
            dj["heartbeat_interval_ms"] = cfg.daemon.heartbeat_interval_ms;
        if (cfg.daemon.heartbeat_timeout_ms != dd.heartbeat_timeout_ms)
            dj["heartbeat_timeout_ms"] = cfg.daemon.heartbeat_timeout_ms;
        if (!dj.empty()) j["daemon"] = dj;

        WebConfig wd;
        nlohmann::json wj = nlohmann::json::object();
        if (cfg.web.enabled != wd.enabled)
            wj["enabled"] = cfg.web.enabled;
        if (cfg.web.bind != wd.bind)
            wj["bind"] = cfg.web.bind;
        if (cfg.web.port != wd.port)
            wj["port"] = cfg.web.port;
        if (!cfg.web.static_dir.empty())
            wj["static_dir"] = cfg.web.static_dir;
        if (!wj.empty()) j["web"] = wj;

        MemoryConfig mem_d;
        nlohmann::json memj = nlohmann::json::object();
        if (cfg.memory.enabled != mem_d.enabled)
            memj["enabled"] = cfg.memory.enabled;
        if (cfg.memory.max_index_bytes != mem_d.max_index_bytes)
            memj["max_index_bytes"] = cfg.memory.max_index_bytes;
        if (!memj.empty()) j["memory"] = memj;

        ProjectInstructionsConfig pi_d;
        nlohmann::json pij = nlohmann::json::object();
        if (cfg.project_instructions.enabled != pi_d.enabled)
            pij["enabled"] = cfg.project_instructions.enabled;
        if (cfg.project_instructions.max_depth != pi_d.max_depth)
            pij["max_depth"] = cfg.project_instructions.max_depth;
        if (cfg.project_instructions.max_bytes != pi_d.max_bytes)
            pij["max_bytes"] = cfg.project_instructions.max_bytes;
        if (cfg.project_instructions.max_total_bytes != pi_d.max_total_bytes)
            pij["max_total_bytes"] = cfg.project_instructions.max_total_bytes;
        if (cfg.project_instructions.filenames != pi_d.filenames)
            pij["filenames"] = cfg.project_instructions.filenames;
        if (cfg.project_instructions.read_claude_md != pi_d.read_claude_md)
            pij["read_claude_md"] = cfg.project_instructions.read_claude_md;
        if (!pij.empty()) j["project_instructions"] = pij;

        CustomInstructionsConfig ci_d;
        nlohmann::json cij = nlohmann::json::object();
        const std::string custom_text = cfg.custom_instructions.text_snapshot();
        if (custom_text != ci_d.text)
            cij["text"] = custom_text;
        if (!cij.empty()) j["custom_instructions"] = cij;

        if (!cfg.connectors.empty()) {
            j["connectors"] = connectors_to_json(cfg.connectors);
        }

        ModelsDevConfig md;
        nlohmann::json mdj = nlohmann::json::object();
        if (cfg.models_dev.allow_network != md.allow_network)
            mdj["allow_network"] = cfg.models_dev.allow_network;
        if (cfg.models_dev.refresh_on_command_only != md.refresh_on_command_only)
            mdj["refresh_on_command_only"] = cfg.models_dev.refresh_on_command_only;
        if (cfg.models_dev.user_override_path.has_value() &&
            !cfg.models_dev.user_override_path->empty())
            mdj["user_override_path"] = *cfg.models_dev.user_override_path;
        if (!mdj.empty()) j["models_dev"] = mdj;

        InputHistoryConfig ih_d;
        nlohmann::json ihj = nlohmann::json::object();
        if (cfg.input_history.enabled != ih_d.enabled)
            ihj["enabled"] = cfg.input_history.enabled;
        if (cfg.input_history.max_entries != ih_d.max_entries)
            ihj["max_entries"] = cfg.input_history.max_entries;
        if (!ihj.empty()) j["input_history"] = ihj;

        AgentLoopConfig al_d;
        nlohmann::json alj = nlohmann::json::object();
        if (cfg.agent_loop.max_iterations != al_d.max_iterations)
            alj["max_iterations"] = cfg.agent_loop.max_iterations;
        // question_policy_explicit 是运行时标记,永不序列化。CLI 覆盖也只改
        // 内存不落盘,这里只在配置值本身偏离默认时写出。
        if (cfg.agent_loop.question_policy != al_d.question_policy)
            alj["question_policy"] = cfg.agent_loop.question_policy;
        if (cfg.agent_loop.question_timeout_seconds != al_d.question_timeout_seconds)
            alj["question_timeout_seconds"] = cfg.agent_loop.question_timeout_seconds;
        if (!alj.empty()) j["agent_loop"] = alj;

        TuiConfig tui_d;
        nlohmann::json tj = nlohmann::json::object();
        if (cfg.tui.alt_screen_mode != tui_d.alt_screen_mode)
            tj["alt_screen_mode"] = cfg.tui.alt_screen_mode;
        if (cfg.tui.page_keys_single_line != tui_d.page_keys_single_line)
            tj["page_keys_single_line"] = cfg.tui.page_keys_single_line;
        if (cfg.tui.theme != tui_d.theme)
            tj["theme"] = cfg.tui.theme;
        if (!tj.empty()) j["tui"] = tj;

        DesktopConfig desk_d;
        DesktopNotificationsConfig dn_d;
        nlohmann::json dnj = nlohmann::json::object();
        if (cfg.desktop.notifications.enabled != dn_d.enabled)
            dnj["enabled"] = cfg.desktop.notifications.enabled;
        if (cfg.desktop.notifications.on_question != dn_d.on_question)
            dnj["on_question"] = cfg.desktop.notifications.on_question;
        if (cfg.desktop.notifications.on_completion != dn_d.on_completion)
            dnj["on_completion"] = cfg.desktop.notifications.on_completion;
        if (cfg.desktop.notifications.suppress_when_focused != dn_d.suppress_when_focused)
            dnj["suppress_when_focused"] = cfg.desktop.notifications.suppress_when_focused;
        nlohmann::json deskj = nlohmann::json::object();
        if (cfg.desktop.close_to_tray != desk_d.close_to_tray)
            deskj["close_to_tray"] = cfg.desktop.close_to_tray;
        if (!dnj.empty()) {
            deskj["notifications"] = dnj;
        }
        // console:schema sparse — 只有非空字段才落盘。
        {
            nlohmann::json cj = nlohmann::json::object();
            if (!cfg.console.shell.empty()) cj["shell"] = cfg.console.shell;
            if (!cfg.console.default_shell.empty()) cj["default_shell"] = cfg.console.default_shell;
            if (!cfg.console.git_bash_path.empty()) cj["git_bash_path"] = cfg.console.git_bash_path;
            if (!cj.empty()) j["console"] = std::move(cj);
        }
        if (!deskj.empty()) {
            j["desktop"] = deskj;
        }

        SessionTitleConfig st_d;
        nlohmann::json stj = nlohmann::json::object();
        if (cfg.session_title.enabled != st_d.enabled)
            stj["enabled"] = cfg.session_title.enabled;
        if (cfg.session_title.model_name != st_d.model_name)
            stj["model_name"] = cfg.session_title.model_name;
        if (cfg.session_title.max_input_bytes != st_d.max_input_bytes)
            stj["max_input_bytes"] = cfg.session_title.max_input_bytes;
        if (cfg.session_title.timeout_ms != st_d.timeout_ms)
            stj["timeout_ms"] = cfg.session_title.timeout_ms;
        if (!stj.empty()) j["session_title"] = stj;

        NetworkConfig net_d;
        nlohmann::json nj = nlohmann::json::object();
        if (cfg.network.proxy_mode != net_d.proxy_mode)
            nj["proxy_mode"] = cfg.network.proxy_mode;
        if (cfg.network.proxy_url != net_d.proxy_url)
            nj["proxy_url"] = cfg.network.proxy_url;
        if (cfg.network.proxy_no_proxy != net_d.proxy_no_proxy)
            nj["proxy_no_proxy"] = cfg.network.proxy_no_proxy;
        if (cfg.network.proxy_probe_enabled != net_d.proxy_probe_enabled)
            nj["proxy_probe_enabled"] = cfg.network.proxy_probe_enabled;
        if (cfg.network.proxy_probe_timeout_ms != net_d.proxy_probe_timeout_ms)
            nj["proxy_probe_timeout_ms"] = cfg.network.proxy_probe_timeout_ms;
        if (!nj.empty()) j["network"] = nj;

        FeaturesConfig features_d;
        nlohmann::json featuresj = nlohmann::json::object();
        if (cfg.features.hooks != features_d.hooks)
            featuresj["hooks"] = cfg.features.hooks;
        if (cfg.features.completed_turn_self_heal != features_d.completed_turn_self_heal)
            featuresj["completed_turn_self_heal"] = cfg.features.completed_turn_self_heal;
        if (!featuresj.empty()) j["features"] = featuresj;

        WebSearchConfig ws_d;
        nlohmann::json wsj = nlohmann::json::object();
        if (cfg.web_search.enabled != ws_d.enabled)
            wsj["enabled"] = cfg.web_search.enabled;
        if (cfg.web_search.backend != ws_d.backend)
            wsj["backend"] = cfg.web_search.backend;
        if (cfg.web_search.api_key != ws_d.api_key)
            wsj["api_key"] = cfg.web_search.api_key;
        if (cfg.web_search.max_results != ws_d.max_results)
            wsj["max_results"] = cfg.web_search.max_results;
        if (cfg.web_search.timeout_ms != ws_d.timeout_ms)
            wsj["timeout_ms"] = cfg.web_search.timeout_ms;
        if (!wsj.empty()) j["web_search"] = wsj;

        nlohmann::json lspj = nlohmann::json::object();
        if (!cfg.lsp.enabled) lspj["enabled"] = false;
        if (!cfg.lsp.servers.empty()) {
            nlohmann::json serversj = nlohmann::json::object();
            for (const auto& [name, entry] : cfg.lsp.servers) {
                nlohmann::json sj = nlohmann::json::object();
                if (entry.disabled) sj["disabled"] = true;
                if (!entry.command.empty()) sj["command"] = entry.command;
                if (!entry.extensions.empty()) sj["extensions"] = entry.extensions;
                if (!entry.env.empty()) sj["env"] = entry.env;
                if (entry.initialization.is_object() && !entry.initialization.empty())
                    sj["initialization"] = entry.initialization;
                serversj[name] = sj;
            }
            lspj["servers"] = serversj;
        }
        if (!lspj.empty()) j["lsp"] = lspj;

        nlohmann::json wtj = nlohmann::json::object();
        if (!cfg.worktree.symlink_directories.empty())
            wtj["symlink_directories"] = cfg.worktree.symlink_directories;
        if (!cfg.worktree.sparse_paths.empty())
            wtj["sparse_paths"] = cfg.worktree.sparse_paths;
        if (!wtj.empty()) j["worktree"] = wtj;

        GitContextConfig gc_d;
        nlohmann::json gcj = nlohmann::json::object();
        if (cfg.git_context.enabled != gc_d.enabled)
            gcj["enabled"] = cfg.git_context.enabled;
        if (cfg.git_context.timeout_ms != gc_d.timeout_ms)
            gcj["timeout_ms"] = cfg.git_context.timeout_ms;
        if (!gcj.empty()) j["git_context"] = gcj;

        RemoteControlConfig rc_d;
        nlohmann::json rcj = nlohmann::json::object();
        if (cfg.remote_control.port != rc_d.port)
            rcj["port"] = cfg.remote_control.port;
        if (cfg.remote_control.token != rc_d.token)
            rcj["token"] = cfg.remote_control.token;
        if (cfg.remote_control.outbound_url != rc_d.outbound_url)
            rcj["outbound_url"] = cfg.remote_control.outbound_url;
        if (cfg.remote_control.default_channel != rc_d.default_channel)
            rcj["default_channel"] = cfg.remote_control.default_channel;
        if (!cfg.remote_control.channels.empty()) {
            nlohmann::json channels = nlohmann::json::object();
            for (const auto& [name, channel] : cfg.remote_control.channels) {
                RemoteControlConfig::ChannelPluginConfig channel_d;
                nlohmann::json cj = nlohmann::json::object();
                if (channel.manifest_path != channel_d.manifest_path)
                    cj["manifest_path"] = channel.manifest_path;
                if (channel.timeout_ms != channel_d.timeout_ms)
                    cj["timeout_ms"] = channel.timeout_ms;
                if (channel.settings.is_object() && !channel.settings.empty())
                    cj["settings"] = channel.settings;
                channels[name] = std::move(cj);
            }
            rcj["channels"] = std::move(channels);
        }
        if (!rcj.empty()) j["remote_control"] = rcj;

        AceBrowserBridgeConfig ab_d;
        nlohmann::json abj = nlohmann::json::object();
        if (cfg.ace_browser_bridge.enabled != ab_d.enabled)
            abj["enabled"] = cfg.ace_browser_bridge.enabled;
        if (cfg.ace_browser_bridge.tool_mode != ab_d.tool_mode)
            abj["tool_mode"] = cfg.ace_browser_bridge.tool_mode;
        if (cfg.ace_browser_bridge.default_mode != ab_d.default_mode)
            abj["default_mode"] = cfg.ace_browser_bridge.default_mode;
        if (cfg.ace_browser_bridge.pointer_speed != ab_d.pointer_speed)
            abj["pointer_speed"] = cfg.ace_browser_bridge.pointer_speed;
        if (cfg.ace_browser_bridge.status_cache_ttl_ms != ab_d.status_cache_ttl_ms)
            abj["status_cache_ttl_ms"] = cfg.ace_browser_bridge.status_cache_ttl_ms;
        if (cfg.ace_browser_bridge.tool_timeout_ms != ab_d.tool_timeout_ms)
            abj["tool_timeout_ms"] = cfg.ace_browser_bridge.tool_timeout_ms;
        if (cfg.ace_browser_bridge.os_pointer_enabled != ab_d.os_pointer_enabled)
            abj["os_pointer_enabled"] = cfg.ace_browser_bridge.os_pointer_enabled;
        if (cfg.ace_browser_bridge.tab_group_enabled != ab_d.tab_group_enabled)
            abj["tab_group_enabled"] = cfg.ace_browser_bridge.tab_group_enabled;
        if (cfg.ace_browser_bridge.operation_overlay_enabled != ab_d.operation_overlay_enabled)
            abj["operation_overlay_enabled"] = cfg.ace_browser_bridge.operation_overlay_enabled;
        if (cfg.ace_browser_bridge.operation_overlay_watchdog_ms !=
            ab_d.operation_overlay_watchdog_ms)
            abj["operation_overlay_watchdog_ms"] =
                cfg.ace_browser_bridge.operation_overlay_watchdog_ms;

        const auto& pc = cfg.ace_browser_bridge.pointer_custom;
        const auto& pc_d = ab_d.pointer_custom;
        nlohmann::json pcj = nlohmann::json::object();
        if (pc.move_duration_ms_min != pc_d.move_duration_ms_min)
            pcj["move_duration_ms_min"] = pc.move_duration_ms_min;
        if (pc.move_duration_ms_max != pc_d.move_duration_ms_max)
            pcj["move_duration_ms_max"] = pc.move_duration_ms_max;
        if (pc.click_hold_ms_min != pc_d.click_hold_ms_min)
            pcj["click_hold_ms_min"] = pc.click_hold_ms_min;
        if (pc.click_hold_ms_max != pc_d.click_hold_ms_max)
            pcj["click_hold_ms_max"] = pc.click_hold_ms_max;
        if (pc.typing_delay_ms_min != pc_d.typing_delay_ms_min)
            pcj["typing_delay_ms_min"] = pc.typing_delay_ms_min;
        if (pc.typing_delay_ms_max != pc_d.typing_delay_ms_max)
            pcj["typing_delay_ms_max"] = pc.typing_delay_ms_max;
        if (pc.jitter_px != pc_d.jitter_px)
            pcj["jitter_px"] = pc.jitter_px;
        if (pc.max_path_points != pc_d.max_path_points)
            pcj["max_path_points"] = pc.max_path_points;
        if (!pcj.empty()) abj["pointer_custom"] = pcj;
        if (!abj.empty()) j["ace_browser_bridge"] = abj;

        UpgradeConfig up_d;
        nlohmann::json upj = nlohmann::json::object();
        if (normalize_upgrade_base_url(cfg.upgrade.base_url) != up_d.base_url)
            upj["base_url"] = normalize_upgrade_base_url(cfg.upgrade.base_url);
        if (cfg.upgrade.timeout_ms != up_d.timeout_ms)
            upj["timeout_ms"] = cfg.upgrade.timeout_ms;
        if (!upj.empty()) j["upgrade"] = upj;
    }

    // --- model profiles ---
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& e : cfg.saved_models) {
        nlohmann::json ej = nlohmann::json::object();
        ej["name"] = e.name;
        ej["provider"] = e.provider;
        ej["model"] = e.model;
        if (!e.base_url.empty()) ej["base_url"] = e.base_url;
        if (!e.api_key.empty()) ej["api_key"] = e.api_key;
        if (e.models_dev_provider_id.has_value() && !e.models_dev_provider_id->empty()) {
            ej["models_dev_provider_id"] = *e.models_dev_provider_id;
        }
        if (e.context_window.has_value() && *e.context_window > 0) {
            ej["context_window"] = *e.context_window;
        }
        if (e.stream_timeout_ms.has_value() && *e.stream_timeout_ms > 0) {
            ej["stream_timeout_ms"] = *e.stream_timeout_ms;
        }
        if (!e.capabilities.empty()) {
            ej["capabilities"] = e.capabilities;
        }
        if (!e.request_headers.empty()) {
            nlohmann::json headers = nlohmann::json::object();
            for (const auto& [k, v] : e.request_headers) headers[k] = v;
            ej["request_headers"] = std::move(headers);
        }
        arr.push_back(std::move(ej));
    }
    j["saved_models"] = std::move(arr);
    if (!cfg.default_model_name.empty()) {
        j["default_model_name"] = cfg.default_model_name;
    }

    if (!cfg.mcp_servers.empty()) {
        nlohmann::json mj = nlohmann::json::object();
        for (const auto& [name, srv] : cfg.mcp_servers) {
            nlohmann::json sj = nlohmann::json::object();
            if (srv.transport == McpTransport::Stdio) {
                // Omit the transport field for stdio so the resulting config
                // stays readable by older acecode builds.
                sj["command"] = srv.command;
                if (!srv.args.empty()) {
                    sj["args"] = srv.args;
                }
                if (!srv.env.empty()) {
                    nlohmann::json ej = nlohmann::json::object();
                    for (const auto& [k, v] : srv.env) ej[k] = v;
                    sj["env"] = ej;
                }
            } else {
                sj["transport"] = (srv.transport == McpTransport::Sse) ? "sse" : "http";
                sj["url"] = srv.url;
                if (srv.sse_endpoint != "/sse") {
                    sj["sse_endpoint"] = srv.sse_endpoint;
                }
                if (!srv.headers.empty()) {
                    nlohmann::json hj = nlohmann::json::object();
                    for (const auto& [k, v] : srv.headers) hj[k] = v;
                    sj["headers"] = hj;
                }
                if (!srv.auth_token.empty()) {
                    sj["auth_token"] = srv.auth_token;
                }
                if (srv.timeout_seconds != 30) {
                    sj["timeout_seconds"] = srv.timeout_seconds;
                }
            }
            // 仅在禁用时写出,启用态保持配置稀疏(与其它布尔字段一致)。
            if (srv.disabled) {
                sj["disabled"] = true;
            }
            mj[name] = sj;
        }
        j["mcp_servers"] = mj;
    }

    return j;
}

} // namespace

void save_config(const AppConfig& cfg) {
    std::string acecode_dir = get_acecode_dir();
    std::string config_path = path_to_utf8(path_from_utf8(acecode_dir) / "config.json");

    fs::path native_acecode_dir = path_from_utf8(acecode_dir);
    if (!fs::exists(native_acecode_dir)) {
        fs::create_directories(native_acecode_dir);
    }

    auto j = build_config_json(cfg);
    std::ofstream ofs(path_from_utf8(config_path));
    if (ofs.is_open()) {
        ofs << j.dump(2) << std::endl;
    }
}

void save_config(const AppConfig& cfg, const std::string& explicit_path) {
    fs::path p = path_from_utf8(explicit_path);
    if (p.has_parent_path() && !fs::exists(p.parent_path())) {
        fs::create_directories(p.parent_path());
    }

    auto j = build_config_json(cfg);
    std::ofstream ofs(p);
    if (ofs.is_open()) {
        ofs << j.dump(2) << std::endl;
    }
}

bool refresh_default_session_preferences_from_config(
    AppConfig& cfg,
    const std::string& explicit_path,
    std::string* error) {
    if (error) error->clear();

    std::string config_path = explicit_path;
    if (config_path.empty()) {
        config_path = path_to_utf8(path_from_utf8(get_acecode_dir()) / "config.json");
    }

    fs::path native_path = path_from_utf8(config_path);
    std::error_code ec;
    if (!fs::exists(native_path, ec) || ec) {
        return true;
    }

    std::ifstream ifs(native_path);
    if (!ifs.is_open()) {
        if (error) *error = "failed to open config file: " + config_path;
        return false;
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(ifs);
    } catch (const std::exception& e) {
        if (error) *error = std::string("failed to parse config file: ") + e.what();
        return false;
    }
    if (!j.is_object()) {
        if (error) *error = "config root must be a JSON object";
        return false;
    }

    AppConfig next = cfg;
    if (j.contains("saved_models")) {
        std::string err;
        auto parsed = parse_saved_models(j["saved_models"], err);
        if (!parsed.has_value()) {
            if (error) *error = err;
            return false;
        }
        next.saved_models = std::move(*parsed);
    }
    if (j.contains("default_model_name") && j["default_model_name"].is_string()) {
        next.default_model_name = j["default_model_name"].get<std::string>();
    }
    if (j.contains("default_permission_mode") &&
        j["default_permission_mode"].is_string()) {
        next.default_permission_mode = normalize_permission_mode_name(
            j["default_permission_mode"].get<std::string>());
    } else if (!j.contains("default_permission_mode")) {
        next.default_permission_mode = "default";
    }

    if (!next.saved_models.empty()) {
        std::string err;
        if (!validate_saved_models(next.saved_models, next.default_model_name, err)) {
            if (error) *error = err;
            return false;
        }
        sanitize_disabled_model_providers(next);
    } else if (!next.default_model_name.empty()) {
        LOG_WARN("[config] default_model_name ignored because saved_models is empty: " +
                 next.default_model_name);
        next.default_model_name.clear();
    }

    cfg.saved_models = std::move(next.saved_models);
    cfg.default_model_name = std::move(next.default_model_name);
    cfg.default_permission_mode = std::move(next.default_permission_mode);
    return true;
}

} // namespace acecode
