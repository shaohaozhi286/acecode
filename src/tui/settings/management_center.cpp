#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#endif

#include "management_center.hpp"

#include "../../commands/command_registry.hpp"
#include "../../config/config_mutation.hpp"
#include "../../hooks/hook_manager.hpp"
#include "../../hooks/hook_registry.hpp"
#include "../../skills/skill_commands.hpp"
#include "../../skills/skill_init.hpp"
#include "../../skills/skill_registry.hpp"
#include "../../tool/mcp_manager.hpp"
#include "../../tool/tool_executor.hpp"
#include "../../utils/utf8_path.hpp"
#include "../theme_palette.hpp"

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#  include <windows.h>
#  include <shellapi.h>
#else
#  include <sys/types.h>
#  include <unistd.h>
#endif

namespace acecode::tui::settings {
namespace {

using namespace ftxui;
namespace fs = std::filesystem;

constexpr const char* kRedactedSecret = "<redacted>";

std::string lower_ascii(std::string value) {
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string trim_ascii(std::string value) {
    auto visible = [](unsigned char ch) {
        return std::isspace(ch) == 0;
    };
    value.erase(
        value.begin(),
        std::find_if(value.begin(), value.end(), visible));
    value.erase(
        std::find_if(value.rbegin(), value.rend(), visible).base(),
        value.end());
    return value;
}

std::string truncate_middle(
    const std::string& value,
    std::size_t limit);

void replace_all(
    std::string& value,
    const std::string& needle,
    const std::string& replacement) {
    if (needle.empty() || needle == replacement) return;
    std::size_t offset = 0;
    while ((offset = value.find(needle, offset)) != std::string::npos) {
        value.replace(offset, needle.size(), replacement);
        offset += replacement.size();
    }
}

bool is_sensitive_cli_flag(std::string_view flag) {
    const std::string normalized = lower_ascii(std::string(flag));
    return normalized.find("token") != std::string::npos ||
        normalized.find("secret") != std::string::npos ||
        normalized.find("password") != std::string::npos ||
        normalized.find("passwd") != std::string::npos ||
        normalized.find("api-key") != std::string::npos ||
        normalized.find("api_key") != std::string::npos ||
        normalized.find("credential") != std::string::npos ||
        normalized.find("bearer") != std::string::npos;
}

std::vector<std::string> redact_mcp_args(
    const std::vector<std::string>& args) {
    std::vector<std::string> safe;
    safe.reserve(args.size());
    bool redact_next = false;
    for (const auto& arg : args) {
        if (redact_next) {
            safe.push_back(kRedactedSecret);
            redact_next = false;
            continue;
        }
        const std::size_t equals = arg.find('=');
        const std::string_view flag(
            arg.data(),
            equals == std::string::npos ? arg.size() : equals);
        if (!is_sensitive_cli_flag(flag)) {
            safe.push_back(arg);
            continue;
        }
        if (equals == std::string::npos) {
            safe.push_back(arg);
            redact_next = true;
        } else {
            safe.push_back(
                arg.substr(0, equals + 1) + kRedactedSecret);
        }
    }
    return safe;
}

std::string redact_url_value(std::string value) {
    const std::size_t scheme = value.find("://");
    if (scheme != std::string::npos) {
        const std::size_t authority_start = scheme + 3;
        const std::size_t authority_end =
            value.find_first_of("/?#", authority_start);
        const std::size_t at = value.find('@', authority_start);
        if (at != std::string::npos &&
            (authority_end == std::string::npos || at < authority_end)) {
            value.erase(authority_start, at + 1 - authority_start);
        }
    }
    const std::size_t suffix = value.find_first_of("?#");
    if (suffix != std::string::npos) {
        value =
            value.substr(0, suffix + 1) + kRedactedSecret;
    }
    return value;
}

std::string safe_mcp_locator(
    const AppConfig* app_config,
    const McpServerInfo& row) {
    if (!app_config) return "(configuration unavailable)";
    const auto found = app_config->mcp_servers.find(row.name);
    if (found == app_config->mcp_servers.end()) {
        return "(configuration unavailable)";
    }
    const auto& config = found->second;
    if (config.transport == McpTransport::Stdio) {
        const std::string command =
            config.command.empty() ? "(command not configured)"
                                   : config.command;
        if (config.args.empty()) return command;
        return command + " (" + std::to_string(config.args.size()) +
            (config.args.size() == 1 ? " argument hidden)"
                                     : " arguments hidden)");
    }
    return redact_url_value(config.url + config.sse_endpoint);
}

std::string safe_mcp_error(
    const AppConfig* app_config,
    const McpServerInfo& row) {
    if (row.error.empty() || !app_config) return row.error;
    const auto found = app_config->mcp_servers.find(row.name);
    if (found == app_config->mcp_servers.end()) {
        return truncate_middle(row.error, 240);
    }
    const auto& config = found->second;
    std::string safe = row.error;
    for (const auto& [name, secret] : config.env) {
        (void)name;
        replace_all(safe, secret, kRedactedSecret);
    }
    for (const auto& [name, secret] : config.headers) {
        (void)name;
        replace_all(safe, secret, kRedactedSecret);
    }
    replace_all(safe, config.auth_token, kRedactedSecret);
    const auto safe_args = redact_mcp_args(config.args);
    for (std::size_t i = 0; i < config.args.size(); ++i) {
        if (safe_args[i] != config.args[i]) {
            replace_all(safe, config.args[i], safe_args[i]);
        }
    }
    const std::string safe_url = redact_url_value(config.url);
    if (safe_url != config.url) {
        replace_all(safe, config.url, safe_url);
    }
    const std::string safe_endpoint =
        redact_url_value(config.sse_endpoint);
    if (safe_endpoint != config.sse_endpoint) {
        replace_all(safe, config.sse_endpoint, safe_endpoint);
    }
    return truncate_middle(safe, 240);
}

std::string truncate_middle(const std::string& value, std::size_t limit) {
    if (value.size() <= limit || limit < 7) return value;
    const std::size_t left = (limit - 3) / 2;
    const std::size_t right = limit - 3 - left;
    return value.substr(0, left) + "..." +
        value.substr(value.size() - right);
}

Element page_heading(const std::string& title, const std::string& description) {
    return vbox({
        text(title) | bold | color(theme().ui.text_primary),
        paragraph(description) | color(theme().ui.text_secondary),
        separator() | color(theme().ui.text_dim),
    });
}

Element status_line(const std::string& value, bool error) {
    if (value.empty()) return text("");
    return hbox({
        text(error ? "Error: " : "Status: ") | bold,
        paragraph(value),
    }) | color(error ? theme().semantic.error : theme().semantic.success);
}

InputOption compact_input_option() {
    InputOption option;
    option.multiline = false;
    option.transform = [](InputState state) {
        const Color foreground =
            state.focused
                ? theme().ui.selection_fg
                : (state.is_placeholder
                       ? theme().ui.text_secondary
                       : theme().ui.text_primary);
        const Color background =
            state.focused
                ? theme().ui.selection_bg
                : theme().ui.input_bg;
        Element value = state.element | color(foreground);
        if (state.is_placeholder && !state.focused) {
            value = std::move(value) | dim;
        } else if (state.hovered && !state.focused) {
            value = std::move(value) | underlined;
        }
        return hbox({
            text(" "),
            std::move(value),
            filler(),
            text(" "),
        }) | bgcolor(background) | size(HEIGHT, EQUAL, 1);
    };
    return option;
}

Element compact_input_element(const Component& component) {
    return component->Render() |
        size(HEIGHT, EQUAL, 1) |
        flex;
}

Element badge(const std::string& value, Color foreground, Color background) {
    return text(" " + value + " ") |
        color(foreground) | bgcolor(background);
}

bool open_path(const std::string& path) {
#ifdef _WIN32
    const std::wstring wide = path_from_utf8(path).wstring();
    const auto result = reinterpret_cast<std::intptr_t>(
        ::ShellExecuteW(
            nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    return result > 32;
#else
    const pid_t pid = ::fork();
    if (pid < 0) return false;
    if (pid == 0) {
#  ifdef __APPLE__
        ::execlp("open", "open", path.c_str(), static_cast<char*>(nullptr));
#  else
        ::execlp(
            "xdg-open", "xdg-open", path.c_str(),
            static_cast<char*>(nullptr));
#  endif
        _exit(127);
    }
    return true;
#endif
}

std::string mcp_state_name(McpServerState state) {
    switch (state) {
        case McpServerState::Starting: return "starting";
        case McpServerState::Connected: return "connected";
        case McpServerState::Disabled: return "disabled";
        case McpServerState::Failed: return "failed";
        case McpServerState::Cancelled: return "cancelled";
        case McpServerState::TimedOut: return "timed out";
    }
    return "unknown";
}

std::string skill_scope(
    const SkillMetadata& skill,
    const AppConfig& config,
    const std::string& cwd) {
    const std::string root = lower_ascii(
        path_to_utf8(skill.scan_root.lexically_normal()));
    for (const auto& project_root : project_skill_scan_roots(config, cwd)) {
        if (root == lower_ascii(
                        path_to_utf8(project_root.lexically_normal()))) {
            return "project";
        }
    }
    return "user";
}

nlohmann::json mcp_config_to_safe_json(const McpServerConfig& config) {
    nlohmann::json json;
    switch (config.transport) {
        case McpTransport::Stdio: json["transport"] = "stdio"; break;
        case McpTransport::Sse: json["transport"] = "sse"; break;
        case McpTransport::Http: json["transport"] = "http"; break;
    }
    if (!config.command.empty()) json["command"] = config.command;
    if (!config.args.empty()) json["args"] = redact_mcp_args(config.args);
    if (!config.env.empty()) {
        nlohmann::json env = nlohmann::json::object();
        for (const auto& [name, value] : config.env) {
            (void)value;
            env[name] = kRedactedSecret;
        }
        json["env"] = std::move(env);
    }
    if (!config.url.empty()) {
        json["url"] = redact_url_value(config.url);
    }
    if (!config.sse_endpoint.empty()) {
        json["sse_endpoint"] =
            redact_url_value(config.sse_endpoint);
    }
    if (!config.headers.empty()) {
        nlohmann::json headers = nlohmann::json::object();
        for (const auto& [name, value] : config.headers) {
            (void)value;
            headers[name] = kRedactedSecret;
        }
        json["headers"] = std::move(headers);
    }
    if (!config.auth_token.empty()) {
        json["auth_token"] = kRedactedSecret;
    }
    json["timeout_seconds"] = config.timeout_seconds;
    json["disabled"] = config.disabled;
    return json;
}

bool parse_string_map(
    const nlohmann::json& value,
    std::map<std::string, std::string>& output,
    const std::map<std::string, std::string>& existing,
    std::string& error,
    const char* field) {
    if (!value.is_object()) {
        error = std::string(field) + " must be a JSON object";
        return false;
    }
    output.clear();
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (!it.value().is_string()) {
            error = std::string(field) + " values must be strings";
            return false;
        }
        std::string item = it.value().get<std::string>();
        if (item == kRedactedSecret) {
            const auto old = existing.find(it.key());
            if (old != existing.end()) item = old->second;
        }
        output[it.key()] = std::move(item);
    }
    return true;
}

std::optional<McpServerConfig> parse_mcp_editor(
    const std::string& text,
    const McpServerConfig& existing,
    std::string& error) {
    try {
        const auto json = nlohmann::json::parse(text);
        if (!json.is_object()) {
            error = "MCP definition must be a JSON object";
            return std::nullopt;
        }
        McpServerConfig config = existing;
        const std::string transport =
            json.value("transport", std::string("stdio"));
        if (transport == "stdio") {
            config.transport = McpTransport::Stdio;
        } else if (transport == "sse") {
            config.transport = McpTransport::Sse;
        } else if (transport == "http") {
            config.transport = McpTransport::Http;
        } else {
            error = "transport must be stdio, sse, or http";
            return std::nullopt;
        }
        if (json.contains("command")) {
            if (!json["command"].is_string()) {
                error = "command must be a string";
                return std::nullopt;
            }
            config.command = json["command"].get<std::string>();
        }
        if (json.contains("args")) {
            if (!json["args"].is_array()) {
                error = "args must be an array of strings";
                return std::nullopt;
            }
            config.args.clear();
            std::size_t index = 0;
            for (const auto& item : json["args"]) {
                if (!item.is_string()) {
                    error = "args must be an array of strings";
                    return std::nullopt;
                }
                std::string argument = item.get<std::string>();
                const bool whole_value_redacted =
                    argument == kRedactedSecret;
                const std::string marker =
                    std::string("=") + kRedactedSecret;
                const bool assigned_value_redacted =
                    argument.size() >= marker.size() &&
                    argument.compare(
                        argument.size() - marker.size(),
                        marker.size(),
                        marker) == 0;
                if (whole_value_redacted || assigned_value_redacted) {
                    if (index >= existing.args.size()) {
                        error =
                            "redacted args must keep an existing value";
                        return std::nullopt;
                    }
                    if (whole_value_redacted) {
                        argument = existing.args[index];
                    } else {
                        const std::size_t equals = argument.find('=');
                        const std::size_t existing_equals =
                            existing.args[index].find('=');
                        if (equals == std::string::npos ||
                            existing_equals == std::string::npos ||
                            argument.substr(0, equals) !=
                                existing.args[index].substr(
                                    0, existing_equals)) {
                            error =
                                "redacted args must keep their existing flag";
                            return std::nullopt;
                        }
                        argument = existing.args[index];
                    }
                }
                config.args.push_back(std::move(argument));
                ++index;
            }
        }
        if (json.contains("env") &&
            !parse_string_map(
                json["env"], config.env, existing.env, error, "env")) {
            return std::nullopt;
        }
        if (json.contains("url")) {
            if (!json["url"].is_string()) {
                error = "url must be a string";
                return std::nullopt;
            }
            const std::string edited =
                json["url"].get<std::string>();
            const std::string redacted_existing =
                redact_url_value(existing.url);
            config.url =
                edited == redacted_existing &&
                    redacted_existing != existing.url
                ? existing.url
                : edited;
        }
        if (json.contains("sse_endpoint")) {
            if (!json["sse_endpoint"].is_string()) {
                error = "sse_endpoint must be a string";
                return std::nullopt;
            }
            const std::string edited =
                json["sse_endpoint"].get<std::string>();
            const std::string redacted_existing =
                redact_url_value(existing.sse_endpoint);
            config.sse_endpoint =
                edited == redacted_existing &&
                    redacted_existing != existing.sse_endpoint
                ? existing.sse_endpoint
                : edited;
        }
        if (json.contains("headers") &&
            !parse_string_map(
                json["headers"],
                config.headers,
                existing.headers,
                error,
                "headers")) {
            return std::nullopt;
        }
        if (json.contains("auth_token")) {
            if (!json["auth_token"].is_string()) {
                error = "auth_token must be a string";
                return std::nullopt;
            }
            const std::string token =
                json["auth_token"].get<std::string>();
            config.auth_token =
                token == kRedactedSecret ? existing.auth_token : token;
        }
        if (json.contains("timeout_seconds")) {
            if (!json["timeout_seconds"].is_number_integer()) {
                error = "timeout_seconds must be an integer";
                return std::nullopt;
            }
            config.timeout_seconds =
                json["timeout_seconds"].get<int>();
        }
        config.disabled = json.value("disabled", config.disabled);
        if (config.timeout_seconds < 1 ||
            config.timeout_seconds > 3600) {
            error = "timeout_seconds must be between 1 and 3600";
            return std::nullopt;
        }
        if (config.transport == McpTransport::Stdio &&
            trim_ascii(config.command).empty()) {
            error = "command is required for stdio transport";
            return std::nullopt;
        }
        if (config.transport != McpTransport::Stdio &&
            trim_ascii(config.url).empty()) {
            error = "url is required for sse/http transport";
            return std::nullopt;
        }
        return config;
    } catch (const std::exception& e) {
        error = std::string("Invalid JSON: ") + e.what();
        return std::nullopt;
    }
}

} // namespace

struct ManagementCenter::Impl {
    explicit Impl(ManagementCenterDependencies dependencies)
        : deps(std::move(dependencies)),
          tabs(
              management_tab_labels().begin(),
              management_tab_labels().end()) {
        build_components();
    }

    ~Impl() {
        shutting_down.store(true);
        ++async_generation;
        for (auto& thread : async_threads) {
            if (thread.joinable()) thread.join();
        }
    }

    ManagementCenterDependencies deps;
    ManagementNavigationModel navigation;
    std::vector<std::string> tabs;
    int tab_index = 0;
    std::string status;
    bool status_error = false;

    std::string skill_filter;
    std::vector<SkillMetadata> skill_catalog;
    std::vector<std::size_t> visible_skill_indexes;
    std::vector<std::string> skill_entries;
    int skill_selected = 0;

    std::string mcp_filter;
    std::vector<McpServerInfo> mcp_rows;
    std::vector<std::size_t> visible_mcp_indexes;
    std::vector<std::string> mcp_entries;
    int mcp_selected = 0;
    bool mcp_editor_open = false;
    bool mcp_editor_dirty = false;
    bool mcp_discard_modal_open = false;
    std::string mcp_editor_name;
    std::string mcp_editor_json;
    std::string mcp_editor_status;

    std::string connector_filter;
    std::vector<std::size_t> visible_connector_indexes;
    std::vector<std::string> connector_entries;
    int connector_selected = 0;

    struct ToolRow {
        ToolDef definition;
        ToolSource source = ToolSource::Builtin;
        bool read_only = false;
    };
    std::string tool_filter;
    std::vector<ToolRow> tool_rows;
    std::vector<std::size_t> visible_tool_indexes;
    std::vector<std::string> tool_entries;
    int tool_selected = 0;

    std::string hook_filter;
    HookRegistrySnapshot hook_snapshot;
    std::vector<std::size_t> visible_hook_indexes;
    std::vector<std::string> hook_entries;
    int hook_selected = 0;
    bool confirm_modal_open = false;
    std::string confirm_title;
    std::string confirm_message;
    std::function<void()> confirm_action;

    std::atomic<bool> shutting_down{false};
    std::atomic<std::uint64_t> async_generation{0};
    std::vector<std::thread> async_threads;

    Component tab_menu;
    Component tab_content;
    Component root;
    Component skill_filter_input;
    Component skill_menu;
    Component skill_toggle_button;
    Component skill_reload_button;
    Component skill_open_button;
    Component skills_page;
    Component mcp_filter_input;
    Component mcp_menu;
    Component mcp_toggle_button;
    Component mcp_reconnect_button;
    Component mcp_edit_button;
    Component mcp_reload_button;
    Component mcp_page;
    Component connector_filter_input;
    Component connector_menu;
    Component connector_toggle_button;
    Component connector_refresh_button;
    Component connectors_page;
    Component tool_filter_input;
    Component tool_menu;
    Component tools_page;
    Component hook_filter_input;
    Component hook_menu;
    Component hook_toggle_button;
    Component hook_trust_button;
    Component hook_reload_button;
    Component hooks_page;
    Component mcp_editor_input;
    Component mcp_editor_save_button;
    Component mcp_editor_cancel_button;
    Component mcp_editor_component;
    Component mcp_discard_modal_component;
    Component confirm_modal_component;

    void post_event() {
        if (deps.post_event) deps.post_event();
    }

    void post_to_ui(std::function<void()> task) {
        if (deps.post_to_ui) {
            deps.post_to_ui(std::move(task));
        } else {
            task();
        }
    }

    void set_status(std::string value, bool error = false) {
        status = std::move(value);
        status_error = error;
        post_event();
    }

    ConfigMutationResult mutate(const ConfigMutator& mutator) {
        auto result = mutate_config(mutator, {}, deps.config);
        if (result.ok && deps.config) {
            *deps.config = result.config;
        }
        return result;
    }

    void refresh_all() {
        refresh_skills();
        refresh_mcp();
        refresh_connectors();
        refresh_tools();
        refresh_hooks();
    }

    void refresh_skills() {
        skill_catalog.clear();
        if (!deps.config) {
            rebuild_skill_entries();
            return;
        }
        AppConfig scan_config = *deps.config;
        scan_config.skills.disabled.clear();
        SkillRegistry catalog;
        initialize_skill_registry(catalog, scan_config, deps.cwd);
        skill_catalog = catalog.list();
        rebuild_skill_entries();
    }

    bool skill_is_disabled(const std::string& name) const {
        if (!deps.config) return false;
        return std::find(
                   deps.config->skills.disabled.begin(),
                   deps.config->skills.disabled.end(),
                   name) != deps.config->skills.disabled.end();
    }

    void rebuild_skill_entries() {
        visible_skill_indexes.clear();
        skill_entries.clear();
        for (std::size_t i = 0; i < skill_catalog.size(); ++i) {
            const auto& skill = skill_catalog[i];
            if (!search_matches(
                    skill_filter,
                    {skill.name, skill.command_key, skill.description,
                     skill.category,
                     path_to_utf8(skill.skill_dir)})) {
                continue;
            }
            const bool disabled = skill_is_disabled(skill.name);
            const std::string scope = deps.config
                ? skill_scope(skill, *deps.config, deps.cwd)
                : "unknown";
            skill_entries.push_back(
                std::string(disabled ? "[off] " : "[on]  ") +
                skill.name + "  (" + scope + ")");
            visible_skill_indexes.push_back(i);
        }
        if (skill_entries.empty()) {
            skill_entries.push_back("      No matching skills");
        }
        skill_selected = std::clamp(
            skill_selected, 0,
            visible_skill_indexes.empty()
                ? 0
                : static_cast<int>(visible_skill_indexes.size()) - 1);
        navigation.page(ManagementTab::Skills).filter = skill_filter;
        navigation.page(ManagementTab::Skills).selected = skill_selected;
    }

    const SkillMetadata* selected_skill() const {
        if (visible_skill_indexes.empty()) return nullptr;
        const std::size_t index = visible_skill_indexes[
            static_cast<std::size_t>(skill_selected)];
        return index < skill_catalog.size() ? &skill_catalog[index] : nullptr;
    }

    void toggle_skill() {
        const SkillMetadata* selected = selected_skill();
        if (!selected || !deps.config) {
            set_status("Select a skill first.", true);
            return;
        }
        const std::string name = selected->name;
        const bool disable = !skill_is_disabled(name);
        const auto result = mutate(
            [name, disable](AppConfig& config, std::string&) {
                auto& disabled = config.skills.disabled;
                disabled.erase(
                    std::remove(
                        disabled.begin(), disabled.end(), name),
                    disabled.end());
                if (disable) disabled.push_back(name);
                std::sort(disabled.begin(), disabled.end());
                disabled.erase(
                    std::unique(disabled.begin(), disabled.end()),
                    disabled.end());
                return true;
            });
        if (!result.ok) {
            set_status(result.error, true);
            return;
        }
        if (deps.skills && deps.config) {
            initialize_skill_registry(
                *deps.skills, *deps.config, deps.cwd);
            if (deps.commands) {
                reload_skill_commands(*deps.commands, *deps.skills);
            }
        }
        refresh_skills();
        set_status(
            std::string(disable ? "Disabled skill: " : "Enabled skill: ") +
            name);
    }

    void reload_skills() {
        if (deps.skills && deps.config) {
            initialize_skill_registry(
                *deps.skills, *deps.config, deps.cwd);
            if (deps.commands) {
                reload_skill_commands(*deps.commands, *deps.skills);
            }
        }
        refresh_skills();
        set_status(
            "Reloaded " + std::to_string(skill_catalog.size()) +
            " installed skills.");
    }

    void refresh_mcp() {
        mcp_rows = deps.mcp ? deps.mcp->list_servers()
                            : std::vector<McpServerInfo>{};
        rebuild_mcp_entries();
    }

    void rebuild_mcp_entries() {
        visible_mcp_indexes.clear();
        mcp_entries.clear();
        for (std::size_t i = 0; i < mcp_rows.size(); ++i) {
            const auto& row = mcp_rows[i];
            const std::string state = mcp_state_name(row.state);
            if (!search_matches(
                    mcp_filter,
                    {row.name, state, row.transport, row.command_line,
                     row.error})) {
                continue;
            }
            mcp_entries.push_back(
                row.name + "  [" + state + "]  " +
                row.transport + "  tools=" +
                std::to_string(row.tool_count));
            visible_mcp_indexes.push_back(i);
        }
        if (mcp_entries.empty()) {
            mcp_entries.push_back("  No matching MCP servers");
        }
        mcp_selected = std::clamp(
            mcp_selected, 0,
            visible_mcp_indexes.empty()
                ? 0
                : static_cast<int>(visible_mcp_indexes.size()) - 1);
        navigation.page(ManagementTab::McpServers).filter = mcp_filter;
        navigation.page(ManagementTab::McpServers).selected = mcp_selected;
    }

    const McpServerInfo* selected_mcp() const {
        if (visible_mcp_indexes.empty()) return nullptr;
        const std::size_t index = visible_mcp_indexes[
            static_cast<std::size_t>(mcp_selected)];
        return index < mcp_rows.size() ? &mcp_rows[index] : nullptr;
    }

    bool persist_mcp_disabled(const std::string& name, bool disabled) {
        const auto result = mutate(
            [name, disabled](AppConfig& config, std::string& error) {
                auto it = config.mcp_servers.find(name);
                if (it == config.mcp_servers.end()) {
                    error = "MCP server is not present in config";
                    return false;
                }
                if (it->second.disabled == disabled) return false;
                it->second.disabled = disabled;
                return true;
            });
        if (!result.ok) {
            set_status(result.error, true);
            return false;
        }
        return true;
    }

    void toggle_mcp() {
        const McpServerInfo* row = selected_mcp();
        if (!row || !deps.mcp || !deps.tools) {
            set_status("Select an MCP server first.", true);
            return;
        }
        const std::string name = row->name;
        const bool enable = row->state == McpServerState::Disabled ||
            row->state == McpServerState::Failed ||
            row->state == McpServerState::Cancelled ||
            row->state == McpServerState::TimedOut;
        if (!persist_mcp_disabled(name, !enable)) return;
        const bool changed = enable
            ? deps.mcp->enable(name, *deps.tools)
            : deps.mcp->disable(name, *deps.tools);
        refresh_mcp();
        set_status(
            changed
                ? std::string(enable ? "Enabled MCP server: "
                                     : "Disabled MCP server: ") +
                      name
                : "MCP server state did not change.");
    }

    void reconnect_mcp() {
        const McpServerInfo* row = selected_mcp();
        if (!row || !deps.mcp || !deps.tools) {
            set_status("Select an MCP server first.", true);
            return;
        }
        const std::string name = row->name;
        const bool started = deps.mcp->reconnect(name, *deps.tools);
        refresh_mcp();
        set_status(
            started ? "Reconnecting MCP server: " + name
                    : "MCP reconnect could not be started.",
            !started);
    }

    void open_mcp_editor() {
        const McpServerInfo* row = selected_mcp();
        if (!row || !deps.config) {
            set_status("Select an MCP server first.", true);
            return;
        }
        const auto it = deps.config->mcp_servers.find(row->name);
        if (it == deps.config->mcp_servers.end()) {
            set_status("MCP server is not present in config.", true);
            return;
        }
        mcp_editor_name = row->name;
        mcp_editor_json = mcp_config_to_safe_json(it->second).dump(2);
        mcp_editor_status.clear();
        mcp_editor_dirty = false;
        mcp_editor_open = true;
    }

    void close_mcp_editor_now() {
        mcp_editor_dirty = false;
        mcp_discard_modal_open = false;
        mcp_editor_open = false;
    }

    void request_close_mcp_editor() {
        if (mcp_editor_dirty) {
            mcp_discard_modal_open = true;
            return;
        }
        close_mcp_editor_now();
    }

    void save_mcp_editor() {
        if (!deps.config) return;
        const auto current =
            deps.config->mcp_servers.find(mcp_editor_name);
        if (current == deps.config->mcp_servers.end()) {
            mcp_editor_status = "Server no longer exists.";
            return;
        }
        std::string error;
        const auto parsed =
            parse_mcp_editor(mcp_editor_json, current->second, error);
        if (!parsed.has_value()) {
            mcp_editor_status = std::move(error);
            return;
        }
        const std::string name = mcp_editor_name;
        const McpServerConfig value = *parsed;
        const auto result = mutate(
            [name, value](AppConfig& config, std::string&) {
                config.mcp_servers[name] = value;
                return true;
            });
        if (!result.ok) {
            mcp_editor_status = result.error;
            return;
        }
        mcp_editor_dirty = false;
        mcp_editor_open = false;
        refresh_mcp();
        set_status(
            "Saved MCP definition for " + name +
            ". Reconnect uses the currently loaded process definition; "
            "restart ACECode to load structural edits.");
    }

    void refresh_connectors() {
        rebuild_connector_entries();
    }

    void rebuild_connector_entries() {
        visible_connector_indexes.clear();
        connector_entries.clear();
        if (deps.config) {
            for (std::size_t i = 0;
                 i < deps.config->connectors.size();
                 ++i) {
                const auto& connector = deps.config->connectors[i];
                if (!search_matches(
                        connector_filter,
                        {connector.id, connector.name,
                         connector.description,
                         connector.auth_error_base_url_prefix})) {
                    continue;
                }
                connector_entries.push_back(
                    std::string(connector.enabled ? "[on]  " : "[off] ") +
                    (connector.name.empty() ? connector.id
                                            : connector.name));
                visible_connector_indexes.push_back(i);
            }
        }
        if (connector_entries.empty()) {
            connector_entries.push_back("      No matching connectors");
        }
        connector_selected = std::clamp(
            connector_selected, 0,
            visible_connector_indexes.empty()
                ? 0
                : static_cast<int>(visible_connector_indexes.size()) - 1);
        navigation.page(ManagementTab::Connectors).filter =
            connector_filter;
        navigation.page(ManagementTab::Connectors).selected =
            connector_selected;
    }

    const ConnectorConfig* selected_connector() const {
        if (!deps.config || visible_connector_indexes.empty()) {
            return nullptr;
        }
        const std::size_t index = visible_connector_indexes[
            static_cast<std::size_t>(connector_selected)];
        return index < deps.config->connectors.size()
            ? &deps.config->connectors[index]
            : nullptr;
    }

    void toggle_connector() {
        const ConnectorConfig* connector = selected_connector();
        if (!connector) {
            set_status("Select a connector first.", true);
            return;
        }
        const std::string id = connector->id;
        const bool enabled = !connector->enabled;
        ++async_generation;
        const auto result = mutate(
            [id, enabled](AppConfig& config, std::string& error) {
                auto it = std::find_if(
                    config.connectors.begin(),
                    config.connectors.end(),
                    [&id](const ConnectorConfig& item) {
                        return item.id == id;
                    });
                if (it == config.connectors.end()) {
                    error = "connector is no longer configured";
                    return false;
                }
                if (it->enabled == enabled) return false;
                it->enabled = enabled;
                return true;
            });
        if (!result.ok) {
            set_status(result.error, true);
            return;
        }
        refresh_connectors();
        set_status(
            std::string(enabled ? "Enabled connector: "
                                : "Disabled connector: ") +
            id);
    }

    void refresh_tools() {
        tool_rows.clear();
        if (deps.tools) {
            for (const auto& definition :
                 deps.tools->get_tool_definitions_by_source(
                     ToolSource::Builtin)) {
                tool_rows.push_back({
                    definition,
                    ToolSource::Builtin,
                    deps.tools->is_read_only(definition.name),
                });
            }
            for (const auto& definition :
                 deps.tools->get_tool_definitions_by_source(
                     ToolSource::Mcp)) {
                tool_rows.push_back({
                    definition,
                    ToolSource::Mcp,
                    deps.tools->is_read_only(definition.name),
                });
            }
        }
        std::sort(
            tool_rows.begin(), tool_rows.end(),
            [](const ToolRow& lhs, const ToolRow& rhs) {
                if (lhs.source != rhs.source) {
                    return lhs.source < rhs.source;
                }
                return lhs.definition.name < rhs.definition.name;
            });
        rebuild_tool_entries();
    }

    void rebuild_tool_entries() {
        visible_tool_indexes.clear();
        tool_entries.clear();
        for (std::size_t i = 0; i < tool_rows.size(); ++i) {
            const auto& row = tool_rows[i];
            const std::string source =
                row.source == ToolSource::Builtin ? "builtin" : "mcp";
            if (!search_matches(
                    tool_filter,
                    {row.definition.name, row.definition.description,
                     source})) {
                continue;
            }
            tool_entries.push_back(
                row.definition.name + "  (" + source + ")  " +
                (row.read_only ? "read-only" : "permissioned"));
            visible_tool_indexes.push_back(i);
        }
        if (tool_entries.empty()) {
            tool_entries.push_back("  No matching tools");
        }
        tool_selected = std::clamp(
            tool_selected, 0,
            visible_tool_indexes.empty()
                ? 0
                : static_cast<int>(visible_tool_indexes.size()) - 1);
        navigation.page(ManagementTab::Tools).filter = tool_filter;
        navigation.page(ManagementTab::Tools).selected = tool_selected;
    }

    const ToolRow* selected_tool() const {
        if (visible_tool_indexes.empty()) return nullptr;
        const std::size_t index = visible_tool_indexes[
            static_cast<std::size_t>(tool_selected)];
        return index < tool_rows.size() ? &tool_rows[index] : nullptr;
    }

    void refresh_hooks() {
        if (!deps.hooks || !deps.config) {
            hook_snapshot = {};
            rebuild_hook_entries();
            return;
        }
        std::string error;
        HookTrustStore store = load_hook_trust_store_from_path(
            default_hook_trust_state_path(), &error);
        HookLoadOptions options;
        options.feature_enabled = deps.config->features.hooks;
        options.cwd = deps.cwd;
        options.project_trusted = true;
        hook_snapshot = load_hook_registry(options, &store);
        deps.hooks->refresh_registry(hook_snapshot);
        rebuild_hook_entries();
        if (!error.empty()) set_status(error, true);
    }

    void rebuild_hook_entries() {
        visible_hook_indexes.clear();
        hook_entries.clear();
        for (std::size_t i = 0; i < hook_snapshot.hooks.size(); ++i) {
            const auto& hook = hook_snapshot.hooks[i];
            const std::string state =
                hook_trust_status_name(hook.trust_status);
            if (!search_matches(
                    hook_filter,
                    {hook.id, hook.event_name, hook.matcher,
                     hook.source_path, state,
                     hook.command.command})) {
                continue;
            }
            hook_entries.push_back(
                hook.event_name + "  [" + state + "]  " +
                hook_source_scope_name(hook.source_scope));
            visible_hook_indexes.push_back(i);
        }
        if (hook_entries.empty()) {
            hook_entries.push_back("  No matching hooks");
        }
        hook_selected = std::clamp(
            hook_selected, 0,
            visible_hook_indexes.empty()
                ? 0
                : static_cast<int>(visible_hook_indexes.size()) - 1);
        navigation.page(ManagementTab::Hooks).filter = hook_filter;
        navigation.page(ManagementTab::Hooks).selected = hook_selected;
    }

    const NormalizedHook* selected_hook() const {
        if (visible_hook_indexes.empty()) return nullptr;
        const std::size_t index = visible_hook_indexes[
            static_cast<std::size_t>(hook_selected)];
        return index < hook_snapshot.hooks.size()
            ? &hook_snapshot.hooks[index]
            : nullptr;
    }

    void apply_hook_trust(
        const NormalizedHook& hook,
        bool trust,
        std::optional<bool> disabled) {
        if (hook.managed) {
            set_status("Managed hooks cannot be changed here.", true);
            return;
        }
        std::string error;
        HookTrustStore store = load_hook_trust_store_from_path(
            default_hook_trust_state_path(), &error);
        if (!error.empty()) {
            set_status(error, true);
            return;
        }
        if (trust) trust_hook_definition(store, hook);
        if (disabled.has_value()) {
            set_hook_disabled(store, hook, *disabled);
        }
        if (!save_hook_trust_store_to_path(
                store, default_hook_trust_state_path(), &error)) {
            set_status(error.empty() ? "Could not save hook trust state."
                                     : error,
                       true);
            return;
        }
        refresh_hooks();
        set_status(
            trust ? "Trusted hook definition: " + hook.id
                  : std::string(*disabled ? "Disabled hook: "
                                          : "Enabled hook: ") +
                        hook.id);
    }

    void update_hook_trust(bool trust, std::optional<bool> disabled) {
        const NormalizedHook* hook = selected_hook();
        if (!hook) {
            set_status("Select a hook first.", true);
            return;
        }
        apply_hook_trust(*hook, trust, disabled);
    }

    void request_trust_selected_hook() {
        const NormalizedHook* selected = selected_hook();
        if (!selected) {
            set_status("Select a hook first.", true);
            return;
        }
        if (selected->managed) {
            set_status("Managed hooks cannot be changed here.", true);
            return;
        }
        const NormalizedHook hook = *selected;
        confirm_title = "Trust hook definition";
        confirm_message =
            "Trust hook '" + hook.id + "' from " + hook.source_path +
            "? This allows its command to run when the matching event "
            "fires.";
        confirm_action = [this, hook]() {
            apply_hook_trust(hook, true, false);
        };
        confirm_modal_open = true;
    }

    Element render_skill_details() const {
        const SkillMetadata* skill = selected_skill();
        if (!skill) {
            return paragraph("No skill selected.") |
                color(theme().ui.text_secondary) | border;
        }
        const bool disabled = skill_is_disabled(skill->name);
        return vbox({
            hbox({
                text(skill->name) | bold |
                    color(theme().ui.text_primary),
                filler(),
                badge(
                    disabled ? "DISABLED" : "ENABLED",
                    theme().ui.badge_fg,
                    disabled ? theme().semantic.warning
                             : theme().ui.badge_bg),
            }),
            separator() | color(theme().ui.text_dim),
            paragraph(skill->description.empty()
                          ? "(No description)"
                          : skill->description),
            text("Command   /" + skill->command_key),
            text(
                "Scope     " +
                (deps.config
                     ? skill_scope(*skill, *deps.config, deps.cwd)
                     : "unknown")),
            text("Category  " +
                 (skill->category.empty() ? "(none)" : skill->category)),
            text("Path      " +
                 truncate_middle(path_to_utf8(skill->skill_dir), 68)),
            text(
                "Support   " +
                std::to_string(
                    deps.skills
                        ? deps.skills->list_supporting_files(
                              skill->name)
                              .size()
                        : 0) +
                " files"),
            render_skill_usage(*skill),
        }) | color(theme().ui.text_muted) | border;
    }

    Element render_skill_usage(const SkillMetadata& skill) const {
        if (!deps.skill_usage || !deps.config) {
            return text("");
        }
        const std::int64_t now_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
        const std::int64_t idle_ms =
            static_cast<std::int64_t>(deps.config->skills.idle_days) *
            24LL * 60 * 60 * 1000;
        const auto summaries = deps.skill_usage->get_summary(
            now_ms, idle_ms);
        std::string status;
        std::uint64_t count = 0;
        std::string last_used;
        for (const auto& s : summaries) {
            if (s.name == skill.name) {
                status = s.pinned   ? "[pinned]"
                         : s.dormant ? "[dormant]"
                                     : "[active]";
                count = s.use_count;
                last_used = s.last_used_at.empty()
                                ? "never"
                                : s.last_used_at.substr(0, 10);
                break;
            }
        }
        std::string line = "Usage     " + status + "  " +
                           std::to_string(count) +
                           " use(s), last " + last_used;
        return text(line);
    }

    Element render_mcp_details() const {
        const McpServerInfo* row = selected_mcp();
        if (!row) {
            return paragraph("No MCP server selected.") |
                color(theme().ui.text_secondary) | border;
        }
        return vbox({
            hbox({
                text(row->name) | bold |
                    color(theme().ui.text_primary),
                filler(),
                badge(
                    mcp_state_name(row->state),
                    theme().ui.badge_fg,
                    row->state == McpServerState::Connected
                        ? theme().semantic.success
                        : theme().semantic.warning),
            }),
            separator() | color(theme().ui.text_dim),
            text("Transport  " + row->transport),
            text("Tools      " + std::to_string(row->tool_count)),
            paragraph(
                "Endpoint   " + safe_mcp_locator(deps.config, *row)),
            row->error.empty()
                ? text("")
                : paragraph(
                      "Last error: " +
                      safe_mcp_error(deps.config, *row)) |
                      color(theme().semantic.error),
        }) | color(theme().ui.text_muted) | border;
    }

    Element render_connector_details() const {
        const ConnectorConfig* connector = selected_connector();
        if (!connector) {
            return paragraph("No connector selected.") |
                color(theme().ui.text_secondary) | border;
        }
        return vbox({
            hbox({
                text(
                    connector->name.empty()
                        ? connector->id
                        : connector->name) |
                    bold | color(theme().ui.text_primary),
                filler(),
                badge(
                    connector->enabled ? "ENABLED" : "DISABLED",
                    theme().ui.badge_fg,
                    connector->enabled
                        ? theme().semantic.success
                        : theme().semantic.warning),
            }),
            separator() | color(theme().ui.text_dim),
            paragraph(
                connector->description.empty()
                    ? "(No description)"
                    : connector->description),
            text("ID               " + connector->id),
            text(
                std::string("First-start auth ") +
                (connector->on_startup.has_value()
                     ? "configured"
                     : "not configured")),
            paragraph(
                "Automatic authentication runs only during the first "
                "daemon startup for this ACECode installation."),
        }) | color(theme().ui.text_muted) | border;
    }

    Element render_tool_details() const {
        const ToolRow* row = selected_tool();
        if (!row) {
            return paragraph("No tool selected.") |
                color(theme().ui.text_secondary) | border;
        }
        Elements content = {
            hbox({
                text(row->definition.name) | bold |
                    color(theme().ui.text_primary),
                filler(),
                badge(
                    row->source == ToolSource::Builtin ? "BUILTIN" : "MCP",
                    theme().ui.badge_fg,
                    theme().ui.badge_bg),
            }),
            separator() | color(theme().ui.text_dim),
            paragraph(row->definition.description.empty()
                          ? "(No description)"
                          : row->definition.description),
            text(
                std::string("Permission       ") +
                (row->read_only ? "read-only / automatic"
                                : "subject to permission mode")),
        };
        content.push_back(
            paragraph(
                "This tool is immutable in this view. Manage MCP tools "
                "from the MCP Servers tab or their owning server."));
        return vbox(std::move(content)) |
            color(theme().ui.text_muted) | border;
    }

    Element render_hook_details() const {
        const NormalizedHook* hook = selected_hook();
        if (!hook) {
            return vbox({
                paragraph("No hook selected."),
                text(
                    std::to_string(hook_snapshot.diagnostics.size()) +
                    " registry diagnostic(s)") |
                    color(theme().ui.text_secondary),
            }) | border;
        }
        Elements diagnostics;
        for (const auto& item : hook->diagnostics) {
            diagnostics.push_back(
                paragraph(
                    hook_diagnostic_severity_name(item.severity) +
                    ": " + item.message) |
                color(
                    item.severity == HookDiagnosticSeverity::Error
                        ? theme().semantic.error
                        : theme().semantic.warning));
        }
        if (diagnostics.empty()) {
            diagnostics.push_back(
                text("No hook diagnostics.") |
                color(theme().ui.text_secondary));
        }
        return vbox({
            hbox({
                text(hook->event_name) | bold |
                    color(theme().ui.text_primary),
                filler(),
                badge(
                    hook_trust_status_name(hook->trust_status),
                    theme().ui.badge_fg,
                    hook->trust_status == HookTrustStatus::Trusted ||
                            hook->trust_status ==
                                HookTrustStatus::ManagedTrusted
                        ? theme().semantic.success
                        : theme().semantic.warning),
            }),
            separator() | color(theme().ui.text_dim),
            text("Hook ID    " + truncate_middle(hook->id, 65)),
            text(
                "Source     " +
                hook_source_scope_name(hook->source_scope)),
            paragraph(
                "Path       " +
                truncate_middle(hook->source_path, 70)),
            text(
                "Kind       " + hook_handler_kind_name(hook->kind)),
            paragraph(
                "Command    " +
                (hook->command.command.empty()
                     ? "(not a command handler)"
                     : hook->command.command)),
            text(
                "Matcher    " +
                (hook->matcher.empty() ? "(all)" : hook->matcher)),
            separator() | color(theme().ui.text_dim),
            vbox(std::move(diagnostics)),
        }) | color(theme().ui.text_muted) | border;
    }

    Element render_footer() const {
        ManagementRowCapabilities capabilities;
        switch (navigation.active_tab()) {
            case ManagementTab::Skills:
                capabilities.toggle = selected_skill() != nullptr;
                capabilities.open = selected_skill() != nullptr;
                break;
            case ManagementTab::McpServers:
                capabilities.toggle = selected_mcp() != nullptr;
                capabilities.edit = selected_mcp() != nullptr;
                capabilities.reconnect = selected_mcp() != nullptr;
                break;
            case ManagementTab::Connectors:
                capabilities.toggle = selected_connector() != nullptr;
                break;
            case ManagementTab::Tools: {
                capabilities.toggle = false;
                break;
            }
            case ManagementTab::Hooks: {
                const NormalizedHook* row = selected_hook();
                capabilities.toggle = row && !row->managed;
                capabilities.trust =
                    row &&
                    row->trust_status == HookTrustStatus::PendingReview;
                break;
            }
            case ManagementTab::Count:
                break;
        }
        const auto actions = management_footer_actions(
            navigation.active_tab(), capabilities);
        Elements elements;
        for (std::size_t i = 0; i < actions.size(); ++i) {
            if (i) {
                elements.push_back(
                    text("  |  ") | color(theme().ui.text_dim));
            }
            std::string key;
            switch (actions[i]) {
                case FooterAction::Filter: key = "/"; break;
                case FooterAction::Toggle: key = "Space"; break;
                case FooterAction::Edit: key = "e"; break;
                case FooterAction::Reconnect: key = "r"; break;
                case FooterAction::Reload: key = "R"; break;
                case FooterAction::Refresh: key = "r"; break;
                case FooterAction::Open: key = "o"; break;
                case FooterAction::Trust: key = "t"; break;
                case FooterAction::Details: key = "Enter"; break;
                case FooterAction::Close: key = "Esc"; break;
                default: key = ""; break;
            }
            elements.push_back(hbox({
                text(key) | bold | color(theme().ui.text_primary),
                text(" " + footer_action_label(actions[i])) |
                    color(theme().ui.text_secondary),
            }));
        }
        return hbox(std::move(elements)) | hcenter;
    }

    Component make_filter(
        std::string* value,
        const std::string& placeholder,
        std::function<void()> on_change) {
        InputOption option = compact_input_option();
        option.content = value;
        option.placeholder = placeholder;
        option.multiline = false;
        option.on_change = std::move(on_change);
        return Input(option);
    }

    Component make_list(
        std::vector<std::string>* entries,
        int* selected) {
        auto option = MenuOption::VerticalAnimated();
        option.entries_option.transform = [](const EntryState& state) {
            Element line = text(state.label);
            if (state.focused) {
                return line | bgcolor(theme().ui.selection_bg) |
                    color(theme().ui.selection_fg);
            }
            return line | color(theme().ui.text_muted);
        };
        return Menu(entries, selected, option);
    }

    void build_components() {
        auto tab_option = MenuOption::HorizontalAnimated();
        tab_option.underline.color_active = theme().ui.accent;
        tab_option.underline.color_inactive = theme().ui.text_dim;
        tab_option.entries_option.transform = [](const EntryState& state) {
            Element value = text(" " + state.label + " ");
            return state.active
                ? value | bold | color(theme().ui.text_primary)
                : value | color(theme().ui.text_secondary);
        };
        tab_option.on_change = [this]() {
            navigation.set_active_tab(
                static_cast<ManagementTab>(tab_index));
            status.clear();
            refresh_current();
        };
        tab_menu = Menu(&tabs, &tab_index, tab_option);

        skill_filter_input = make_filter(
            &skill_filter, "/ to search skills",
            [this]() { rebuild_skill_entries(); });
        skill_menu = make_list(&skill_entries, &skill_selected);
        skill_toggle_button = Button(
            " Enable / disable ", [this]() { toggle_skill(); },
            ButtonOption::Animated());
        skill_reload_button = Button(
            " Reload ", [this]() { reload_skills(); },
            ButtonOption::Animated());
        skill_open_button = Button(
            " Open directory ",
            [this]() {
                const SkillMetadata* skill = selected_skill();
                if (!skill) {
                    set_status("Select a skill first.", true);
                    return;
                }
                const bool opened =
                    open_path(path_to_utf8(skill->skill_dir));
                set_status(
                    opened ? "Opened skill directory."
                           : "Could not open skill directory.",
                    !opened);
            },
            ButtonOption::Animated());
        auto skills_container = Container::Vertical({
            skill_filter_input,
            skill_menu,
            Container::Horizontal({
                skill_toggle_button,
                skill_reload_button,
                skill_open_button,
            }),
        });
        skills_page = Renderer(skills_container, [this]() {
            return vbox({
                page_heading(
                    "Skills",
                    "Installed instruction packages from project, user, "
                    "compatibility, and external roots."),
                hbox({
                    text("Search ") | bold,
                    compact_input_element(skill_filter_input),
                    text(
                        "  " +
                        std::to_string(visible_skill_indexes.size()) +
                        " skills") |
                        color(theme().ui.text_secondary),
                }),
                hbox({
                    skill_menu->Render() | frame |
                        vscroll_indicator |
                        size(WIDTH, EQUAL, 52),
                    separator(),
                    render_skill_details() | flex,
                }) | flex,
                hbox({
                    skill_toggle_button->Render(),
                    skill_reload_button->Render(),
                    skill_open_button->Render(),
                }),
            });
        });

        mcp_filter_input = make_filter(
            &mcp_filter, "/ to search MCP servers",
            [this]() { rebuild_mcp_entries(); });
        mcp_menu = make_list(&mcp_entries, &mcp_selected);
        mcp_toggle_button = Button(
            " Enable / disable ", [this]() { toggle_mcp(); },
            ButtonOption::Animated());
        mcp_reconnect_button = Button(
            " Reconnect ", [this]() { reconnect_mcp(); },
            ButtonOption::Animated());
        mcp_edit_button = Button(
            " Edit JSON ", [this]() { open_mcp_editor(); },
            ButtonOption::Animated());
        mcp_reload_button = Button(
            " Reload status ",
            [this]() {
                refresh_mcp();
                set_status("MCP status refreshed.");
            },
            ButtonOption::Animated());
        auto mcp_container = Container::Vertical({
            mcp_filter_input,
            mcp_menu,
            Container::Horizontal({
                mcp_toggle_button,
                mcp_reconnect_button,
                mcp_edit_button,
                mcp_reload_button,
            }),
        });
        mcp_page = Renderer(mcp_container, [this]() {
            return vbox({
                page_heading(
                    "MCP Servers",
                    "Inspect configured servers, discovered tools, runtime "
                    "state, and connection errors."),
                hbox({
                    text("Search ") | bold,
                    compact_input_element(mcp_filter_input),
                    text(
                        "  " + std::to_string(mcp_rows.size()) +
                        " servers") |
                        color(theme().ui.text_secondary),
                }),
                hbox({
                    mcp_menu->Render() | frame |
                        vscroll_indicator |
                        size(WIDTH, EQUAL, 61),
                    separator(),
                    render_mcp_details() | flex,
                }) | flex,
                hbox({
                    mcp_toggle_button->Render(),
                    mcp_reconnect_button->Render(),
                    mcp_edit_button->Render(),
                    mcp_reload_button->Render(),
                }),
            });
        });

        connector_filter_input = make_filter(
            &connector_filter, "/ to search connectors",
            [this]() { rebuild_connector_entries(); });
        connector_menu =
            make_list(&connector_entries, &connector_selected);
        connector_toggle_button = Button(
            " Enable / disable ", [this]() { toggle_connector(); },
            ButtonOption::Animated());
        connector_refresh_button = Button(
            " Refresh ",
            [this]() {
                refresh_connectors();
                set_status("Connector configuration refreshed.");
            },
            ButtonOption::Animated());
        auto connector_container = Container::Vertical({
            connector_filter_input,
            connector_menu,
            Container::Horizontal({
                connector_toggle_button,
                connector_refresh_button,
            }),
        });
        connectors_page = Renderer(connector_container, [this]() {
            return vbox({
                page_heading(
                    "Connectors",
                    "External integrations and their startup, enable, and "
                    "authentication-recovery lifecycle hooks."),
                hbox({
                    text("Search ") | bold,
                    compact_input_element(connector_filter_input),
                    text(
                        "  " +
                        std::to_string(visible_connector_indexes.size()) +
                        " connectors") |
                        color(theme().ui.text_secondary),
                }),
                hbox({
                    connector_menu->Render() | frame |
                        vscroll_indicator |
                        size(WIDTH, EQUAL, 46),
                    separator(),
                    render_connector_details() | flex,
                }) | flex,
                hbox({
                    connector_toggle_button->Render(),
                    connector_refresh_button->Render(),
                }),
            });
        });

        tool_filter_input = make_filter(
            &tool_filter, "/ to search tools",
            [this]() { rebuild_tool_entries(); });
        tool_menu = make_list(&tool_entries, &tool_selected);
        auto tool_container = Container::Vertical({
            tool_filter_input,
            tool_menu,
        });
        tools_page = Renderer(tool_container, [this]() {
            return vbox({
                page_heading(
                    "Tools",
                    "Effective tool registry grouped by source. Immutable "
                    "tools are informational only."),
                hbox({
                    text("Search ") | bold,
                    compact_input_element(tool_filter_input),
                    text(
                        "  " + std::to_string(tool_rows.size()) +
                        " tools") |
                        color(theme().ui.text_secondary),
                }),
                hbox({
                    tool_menu->Render() | frame |
                        vscroll_indicator |
                        size(WIDTH, EQUAL, 58),
                    separator(),
                    render_tool_details() | flex,
                }) | flex,
            });
        });

        hook_filter_input = make_filter(
            &hook_filter, "/ to search hooks",
            [this]() { rebuild_hook_entries(); });
        hook_menu = make_list(&hook_entries, &hook_selected);
        hook_toggle_button = Button(
            " Enable / disable ",
            [this]() {
                const NormalizedHook* hook = selected_hook();
                if (!hook) {
                    set_status("Select a hook first.", true);
                    return;
                }
                const bool disabled =
                    hook->trust_status != HookTrustStatus::Disabled;
                update_hook_trust(false, disabled);
            },
            ButtonOption::Animated());
        hook_trust_button = Button(
            " Trust ", [this]() { request_trust_selected_hook(); },
            ButtonOption::Animated());
        hook_reload_button = Button(
            " Reload ", [this]() {
                refresh_hooks();
                set_status("Hook registry reloaded.");
            },
            ButtonOption::Animated());
        auto hook_container = Container::Vertical({
            hook_filter_input,
            hook_menu,
            Container::Horizontal({
                hook_toggle_button,
                hook_trust_button,
                hook_reload_button,
            }),
        });
        hooks_page = Renderer(hook_container, [this]() {
            return vbox({
                page_heading(
                    "Hooks",
                    "Normalized hook definitions, source trust, skip reasons, "
                    "and loader diagnostics."),
                hbox({
                    text("Search ") | bold,
                    compact_input_element(hook_filter_input),
                    text(
                        "  " +
                        std::to_string(hook_snapshot.hooks.size()) +
                        " hooks  " +
                        std::to_string(
                            hook_snapshot.diagnostics.size()) +
                        " diagnostics") |
                        color(theme().ui.text_secondary),
                }),
                hbox({
                    hook_menu->Render() | frame |
                        vscroll_indicator |
                        size(WIDTH, EQUAL, 62),
                    separator(),
                    render_hook_details() | flex,
                }) | flex,
                hbox({
                    hook_toggle_button->Render(),
                    hook_trust_button->Render(),
                    hook_reload_button->Render(),
                }),
            });
        });

        tab_content = Container::Tab(
            {
                skills_page,
                mcp_page,
                connectors_page,
                tools_page,
                hooks_page,
            },
            &tab_index);

        InputOption editor_option = InputOption::Spacious();
        editor_option.content = &mcp_editor_json;
        editor_option.placeholder = "{}";
        editor_option.multiline = true;
        editor_option.on_change = [this]() {
            if (mcp_editor_open) mcp_editor_dirty = true;
        };
        mcp_editor_input = Input(editor_option);
        mcp_editor_save_button = Button(
            " Save ", [this]() { save_mcp_editor(); },
            ButtonOption::Animated());
        mcp_editor_cancel_button = Button(
            " Cancel ",
            [this]() { request_close_mcp_editor(); },
            ButtonOption::Animated());
        auto editor_container = Container::Vertical({
            mcp_editor_input,
            Container::Horizontal({
                mcp_editor_save_button,
                mcp_editor_cancel_button,
            }),
        });
        mcp_editor_component = Renderer(editor_container, [this]() {
            return vbox({
                text(
                    " Edit MCP server: " + mcp_editor_name +
                    (mcp_editor_dirty ? "* " : " ")) |
                    bold | hcenter,
                separator(),
                paragraph(
                    "Secret values are redacted. Leave <redacted> unchanged "
                    "to preserve the stored value."),
                mcp_editor_input->Render() |
                    size(HEIGHT, EQUAL, 18) | border |
                    color(theme().ui.border),
                mcp_editor_status.empty()
                    ? text("")
                    : paragraph(mcp_editor_status) |
                          color(theme().semantic.error),
                hbox({
                    mcp_editor_save_button->Render(),
                    mcp_editor_cancel_button->Render(),
                }) | hcenter,
            }) | size(WIDTH, EQUAL, 86) | border |
                color(theme().ui.border);
        });
        mcp_editor_component =
            CatchEvent(mcp_editor_component, [this](Event event) {
                if (event == Event::Escape) {
                    request_close_mcp_editor();
                    return true;
                }
                if (event == Event::CtrlS) {
                    save_mcp_editor();
                    return true;
                }
                return false;
            });

        auto mcp_discard_cancel = Button(
            " Cancel ",
            [this]() { mcp_discard_modal_open = false; },
            ButtonOption::Animated());
        auto mcp_discard_save = Button(
            " Save changes ",
            [this]() {
                save_mcp_editor();
                if (!mcp_editor_open) {
                    mcp_discard_modal_open = false;
                }
            },
            ButtonOption::Animated());
        auto mcp_discard_accept = Button(
            " Discard changes ",
            [this]() { close_mcp_editor_now(); },
            ButtonOption::Animated());
        auto mcp_discard_container = Container::Horizontal({
            mcp_discard_cancel,
            mcp_discard_save,
            mcp_discard_accept,
        });
        mcp_discard_modal_component = Renderer(
            mcp_discard_container,
            [mcp_discard_cancel,
             mcp_discard_save,
             mcp_discard_accept]() {
                return vbox({
                    text(" Unsaved MCP definition ") | bold | hcenter,
                    separator(),
                    paragraph(
                        "Save or discard the MCP JSON edits before "
                        "closing the editor?"),
                    hbox({
                        mcp_discard_cancel->Render(),
                        mcp_discard_save->Render(),
                        mcp_discard_accept->Render(),
                    }) | hcenter,
                }) | size(WIDTH, EQUAL, 68) | border;
            });
        mcp_discard_modal_component =
            CatchEvent(
                mcp_discard_modal_component,
                [this](Event event) {
                    if (event != Event::Escape) return false;
                    mcp_discard_modal_open = false;
                    return true;
                });

        auto confirm_cancel = Button(
            " Cancel ",
            [this]() {
                confirm_modal_open = false;
                confirm_action = {};
            },
            ButtonOption::Animated());
        auto confirm_accept = Button(
            " Trust hook ",
            [this]() {
                auto action = std::move(confirm_action);
                confirm_modal_open = false;
                if (action) action();
            },
            ButtonOption::Animated());
        auto confirm_container = Container::Horizontal({
            confirm_cancel,
            confirm_accept,
        });
        confirm_modal_component = Renderer(
            confirm_container,
            [this, confirm_cancel, confirm_accept]() {
                return vbox({
                    text(" " + confirm_title + " ") | bold | hcenter,
                    separator(),
                    paragraph(confirm_message),
                    hbox({
                        confirm_cancel->Render(),
                        confirm_accept->Render(),
                    }) | hcenter,
                }) | size(WIDTH, EQUAL, 66) | border |
                    color(theme().semantic.warning);
            });
        confirm_modal_component =
            CatchEvent(confirm_modal_component, [this](Event event) {
                if (event != Event::Escape) return false;
                confirm_modal_open = false;
                confirm_action = {};
                return true;
            });

        auto center_container = Container::Vertical({
            tab_menu,
            tab_content,
        });
        root = Renderer(center_container, [this]() {
            return vbox({
                hbox({
                    text(" ACECode Capabilities ") | bold |
                        color(theme().ui.text_primary),
                    filler(),
                    text("[Esc] Close ") |
                        color(theme().ui.text_secondary),
                }),
                separator() | color(theme().ui.border),
                tab_menu->Render() | xframe,
                separator() | color(theme().ui.text_dim),
                tab_content->Render() | flex,
                status_line(status, status_error),
                separator() | color(theme().ui.text_dim),
                render_footer(),
            }) | border | color(theme().ui.border);
        });
        root = CatchEvent(root, [this](Event event) {
            if (event == Event::Escape) {
                if (mcp_editor_open) {
                    request_close_mcp_editor();
                    return true;
                }
                if (deps.request_close) deps.request_close();
                return true;
            }
            if (current_filter_focused()) return false;
            const std::string character =
                lower_ascii(event.character());
            if (character == "/") {
                focus_current_filter();
                return true;
            }
            if (event == Event::Character(' ')) {
                toggle_current();
                return true;
            }
            if (character == "e" &&
                navigation.active_tab() ==
                    ManagementTab::McpServers) {
                open_mcp_editor();
                return true;
            }
            if (character == "r") {
                refresh_current();
                return true;
            }
            if (character == "t" &&
                navigation.active_tab() == ManagementTab::Hooks) {
                request_trust_selected_hook();
                return true;
            }
            if (character == "o" &&
                navigation.active_tab() == ManagementTab::Skills) {
                const SkillMetadata* skill = selected_skill();
                if (skill) {
                    open_path(path_to_utf8(skill->skill_dir));
                }
                return true;
            }
            return false;
        });
        root |= Modal(mcp_editor_component, &mcp_editor_open);
        root |= Modal(
            mcp_discard_modal_component,
            &mcp_discard_modal_open);
        root |= Modal(confirm_modal_component, &confirm_modal_open);
    }

    void focus_current_filter() {
        switch (navigation.active_tab()) {
            case ManagementTab::Skills:
                skill_filter_input->TakeFocus();
                break;
            case ManagementTab::McpServers:
                mcp_filter_input->TakeFocus();
                break;
            case ManagementTab::Connectors:
                connector_filter_input->TakeFocus();
                break;
            case ManagementTab::Tools:
                tool_filter_input->TakeFocus();
                break;
            case ManagementTab::Hooks:
                hook_filter_input->TakeFocus();
                break;
            case ManagementTab::Count:
                break;
        }
    }

    bool current_filter_focused() const {
        switch (navigation.active_tab()) {
            case ManagementTab::Skills:
                return skill_filter_input->Focused();
            case ManagementTab::McpServers:
                return mcp_filter_input->Focused();
            case ManagementTab::Connectors:
                return connector_filter_input->Focused();
            case ManagementTab::Tools:
                return tool_filter_input->Focused();
            case ManagementTab::Hooks:
                return hook_filter_input->Focused();
            case ManagementTab::Count:
                return false;
        }
        return false;
    }

    void toggle_current() {
        switch (navigation.active_tab()) {
            case ManagementTab::Skills: toggle_skill(); break;
            case ManagementTab::McpServers: toggle_mcp(); break;
            case ManagementTab::Connectors: toggle_connector(); break;
            case ManagementTab::Tools: break;
            case ManagementTab::Hooks: {
                const NormalizedHook* hook = selected_hook();
                if (hook) {
                    update_hook_trust(
                        false,
                        hook->trust_status != HookTrustStatus::Disabled);
                }
                break;
            }
            case ManagementTab::Count:
                break;
        }
    }

    void refresh_current() {
        switch (navigation.active_tab()) {
            case ManagementTab::Skills: refresh_skills(); break;
            case ManagementTab::McpServers: refresh_mcp(); break;
            case ManagementTab::Connectors: refresh_connectors(); break;
            case ManagementTab::Tools: refresh_tools(); break;
            case ManagementTab::Hooks: refresh_hooks(); break;
            case ManagementTab::Count: break;
        }
    }
};

ManagementCenter::ManagementCenter(
    ManagementCenterDependencies dependencies)
    : impl_(std::make_unique<Impl>(std::move(dependencies))) {}

ManagementCenter::~ManagementCenter() = default;

ftxui::Component ManagementCenter::component() const {
    return impl_->root;
}

void ManagementCenter::open(ManagementTab tab) {
    impl_->navigation.set_active_tab(tab);
    impl_->tab_index = static_cast<int>(tab);
    impl_->status.clear();
    impl_->status_error = false;
    impl_->refresh_current();
    impl_->post_event();
}

ManagementTab ManagementCenter::active_tab() const {
    return impl_->navigation.active_tab();
}

} // namespace acecode::tui::settings
