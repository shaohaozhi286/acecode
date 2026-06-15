#include "hook_registry.hpp"

#include "../config/config.hpp"
#include "../utils/atomic_file.hpp"
#include "../utils/encoding.hpp"
#include "../utils/paths.hpp"
#include "../utils/sha256.hpp"
#include "../utils/utf8_path.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace fs = std::filesystem;

namespace acecode {
namespace {

constexpr int kDefaultCodexHookTimeoutSeconds = 600;

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string trim_ascii(std::string value) {
    auto is_ws = [](unsigned char ch) { return std::isspace(ch) != 0; };
    while (!value.empty() && is_ws(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && is_ws(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

std::string normalize_source_identity(std::string value) {
    for (char& ch : value) {
        if (ch == '\\') ch = '/';
    }
    return value;
}

HookDiagnostic diag(HookDiagnosticSeverity severity,
                    std::string code,
                    std::string message,
                    const HookSource& source,
                    const std::string& hook_id = {},
                    const std::string& event_name = {}) {
    HookDiagnostic d;
    d.severity = severity;
    d.code = std::move(code);
    d.message = std::move(message);
    d.source_id = source.id;
    d.hook_id = hook_id;
    d.event_name = event_name;
    return d;
}

bool is_known_codex_event(const std::string& key) {
    return key == "SessionStart" ||
        key == "PreToolUse" ||
        key == "PermissionRequest" ||
        key == "PostToolUse" ||
        key == "PreCompact" ||
        key == "PostCompact" ||
        key == "UserPromptSubmit" ||
        key == "Stop" ||
        key == "SubagentStart" ||
        key == "SubagentStop";
}

bool root_looks_like_codex_hooks_object(const nlohmann::json& root) {
    if (!root.is_object()) return false;
    for (auto it = root.begin(); it != root.end(); ++it) {
        if (is_known_codex_event(it.key()) && it.value().is_array()) return true;
    }
    return false;
}

nlohmann::json string_vector_to_json(const std::vector<std::string>& values) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& value : values) arr.push_back(value);
    return arr;
}

nlohmann::json command_spec_to_json(const HookCommandSpec& spec) {
    nlohmann::json j = nlohmann::json::object();
    j["command"] = spec.command;
    j["args"] = string_vector_to_json(spec.args);
    return j;
}

nlohmann::json stable_definition_json(const NormalizedHook& hook) {
    nlohmann::json j = nlohmann::json::object();
    j["event_name"] = hook.event_name;
    j["handler_index"] = hook.handler_index;
    j["kind"] = hook_handler_kind_name(hook.kind);
    j["legacy_direct"] = hook.legacy_direct;
    j["managed"] = hook.managed;
    j["matcher"] = hook.matcher;
    j["matcher_group_index"] = hook.matcher_group_index;
    j["source_format"] = hook_source_format_name(hook.source_format);
    j["source_scope"] = hook_source_scope_name(hook.source_scope);

    if (hook.legacy_direct) {
        const HookDefinition& h = hook.legacy.definition;
        nlohmann::json legacy = nlohmann::json::object();
        legacy["id"] = h.id;
        legacy["event"] = h.event;
        legacy["mode"] = h.mode == HookMode::Async ? "async" : "sync";
        legacy["timeout_ms"] = h.timeout_ms;
        legacy["platforms"] = string_vector_to_json(h.platforms);
        legacy["command"] = command_spec_to_json(h.command);
        nlohmann::json commands = nlohmann::json::object();
        for (const auto& [key, spec] : h.commands_by_platform) {
            commands[key] = command_spec_to_json(spec);
        }
        legacy["commands_by_platform"] = std::move(commands);
        j["legacy"] = std::move(legacy);
    } else {
        nlohmann::json command = nlohmann::json::object();
        command["command"] = hook.command.command;
        command["command_windows"] = hook.command.command_windows;
        command["timeout_seconds"] = hook.command.timeout_seconds;
        command["status_message"] = hook.command.status_message;
        command["async"] = hook.command.async;
        command["config_location"] = hook.command.config_location;
        j["command"] = std::move(command);
        j["skip_reason"] = hook.skip_reason;
    }
    return j;
}

std::string hook_id_from_config_or_fallback(const HookSource& source,
                                            const std::string& configured_id,
                                            const std::string& event_name,
                                            int matcher_group_index,
                                            int handler_index) {
    if (!configured_id.empty()) {
        return source.id + "::" + configured_id;
    }
    return make_stable_hook_id(source, event_name, matcher_group_index, handler_index);
}

void finalize_hook(NormalizedHook& hook) {
    hook.definition_hash = stable_hook_definition_hash(hook);
}

void merge_snapshot(HookRegistrySnapshot& dst, HookRegistrySnapshot src) {
    dst.sources.insert(dst.sources.end(),
                       std::make_move_iterator(src.sources.begin()),
                       std::make_move_iterator(src.sources.end()));
    dst.hooks.insert(dst.hooks.end(),
                     std::make_move_iterator(src.hooks.begin()),
                     std::make_move_iterator(src.hooks.end()));
    dst.diagnostics.insert(dst.diagnostics.end(),
                           std::make_move_iterator(src.diagnostics.begin()),
                           std::make_move_iterator(src.diagnostics.end()));
}

std::optional<nlohmann::json> load_json_file(const std::string& path,
                                             std::string* error) {
    if (error) error->clear();
    std::ifstream ifs(path_from_utf8(path), std::ios::binary);
    if (!ifs.is_open()) {
        if (error) *error = "failed to open " + path;
        return std::nullopt;
    }
    try {
        return nlohmann::json::parse(ifs);
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return std::nullopt;
    }
}

std::string home_dir_for_codex() {
#ifdef _WIN32
    std::string home = getenv_utf8("USERPROFILE");
    if (home.empty()) {
        std::string drive = getenv_utf8("HOMEDRIVE");
        std::string path = getenv_utf8("HOMEPATH");
        if (!drive.empty() && !path.empty()) home = drive + path;
    }
#else
    std::string home = getenv_utf8("HOME");
#endif
    return home;
}

HookSource make_source(HookSourceScope scope,
                       HookSourceFormat format,
                       const std::string& path,
                       bool managed = false,
                       bool project_trusted = true) {
    HookSource source;
    source.scope = scope;
    source.format = format;
    source.path = normalize_source_identity(path);
    source.managed = managed;
    source.project_trusted = project_trusted;
    source.id = make_hook_source_id(scope, format, source.path);
    source.label = source.path.empty() ? source.id : source.path;
    return source;
}

bool source_is_auto_trusted_legacy_user(const NormalizedHook& hook,
                                        bool auto_trust_user_legacy_hooks) {
    return auto_trust_user_legacy_hooks &&
        hook.legacy_direct &&
        hook.source_scope == HookSourceScope::Legacy;
}

} // namespace

std::string hook_source_scope_name(HookSourceScope scope) {
    switch (scope) {
    case HookSourceScope::UserGlobal: return "user_global";
    case HookSourceScope::ProjectLocal: return "project_local";
    case HookSourceScope::Managed: return "managed";
    case HookSourceScope::Plugin: return "plugin";
    case HookSourceScope::Legacy: return "legacy";
    case HookSourceScope::Inline: return "inline";
    }
    return "unknown";
}

std::string hook_source_format_name(HookSourceFormat format) {
    switch (format) {
    case HookSourceFormat::Unknown: return "unknown";
    case HookSourceFormat::AceCodeLegacyJson: return "acecode_legacy_json";
    case HookSourceFormat::CodexJson: return "codex_json";
    case HookSourceFormat::CodexInline: return "codex_inline";
    }
    return "unknown";
}

std::string hook_handler_kind_name(HookHandlerKind kind) {
    switch (kind) {
    case HookHandlerKind::Command: return "command";
    case HookHandlerKind::UnsupportedPrompt: return "unsupported_prompt";
    case HookHandlerKind::UnsupportedAgent: return "unsupported_agent";
    case HookHandlerKind::Unknown: return "unknown";
    }
    return "unknown";
}

std::string hook_trust_status_name(HookTrustStatus status) {
    switch (status) {
    case HookTrustStatus::Trusted: return "trusted";
    case HookTrustStatus::PendingReview: return "pending_review";
    case HookTrustStatus::Disabled: return "disabled";
    case HookTrustStatus::ManagedTrusted: return "managed_trusted";
    case HookTrustStatus::SkippedUnsupported: return "skipped_unsupported";
    }
    return "unknown";
}

std::string hook_diagnostic_severity_name(HookDiagnosticSeverity severity) {
    switch (severity) {
    case HookDiagnosticSeverity::Info: return "info";
    case HookDiagnosticSeverity::Warning: return "warning";
    case HookDiagnosticSeverity::Error: return "error";
    }
    return "warning";
}

std::string make_hook_source_id(HookSourceScope scope,
                                HookSourceFormat format,
                                const std::string& identity) {
    return hook_source_scope_name(scope) + ":" +
        hook_source_format_name(format) + ":" +
        normalize_source_identity(identity);
}

std::string make_stable_hook_id(const HookSource& source,
                                const std::string& event_name,
                                int matcher_group_index,
                                int handler_index) {
    return source.id + "::" + event_name +
        "#" + std::to_string(matcher_group_index) +
        "." + std::to_string(handler_index);
}

std::string stable_hook_definition_hash(const NormalizedHook& hook) {
    return sha256_hex(stable_definition_json(hook).dump());
}

HookRegistrySnapshot parse_codex_hooks_json_source(const nlohmann::json& root,
                                                   HookSource source) {
    HookRegistrySnapshot snapshot;
    source.format = HookSourceFormat::CodexJson;
    source.id = make_hook_source_id(source.scope, source.format, source.path);
    snapshot.sources.push_back(source);

    if (!root.is_object()) {
        auto d = diag(HookDiagnosticSeverity::Error, "BAD_ROOT",
                      "Codex hooks root must be an object", source);
        snapshot.sources.back().diagnostics.push_back(d);
        snapshot.diagnostics.push_back(std::move(d));
        return snapshot;
    }

    const nlohmann::json* hooks_obj = nullptr;
    if (root.contains("hooks") && root["hooks"].is_object()) {
        hooks_obj = &root["hooks"];
    } else if (root_looks_like_codex_hooks_object(root)) {
        hooks_obj = &root;
    }
    if (!hooks_obj) {
        auto d = diag(HookDiagnosticSeverity::Error, "MISSING_HOOKS_OBJECT",
                      "Codex hooks source must contain a hooks object", source);
        snapshot.sources.back().diagnostics.push_back(d);
        snapshot.diagnostics.push_back(std::move(d));
        return snapshot;
    }

    for (auto ev = hooks_obj->begin(); ev != hooks_obj->end(); ++ev) {
        const std::string event_name = ev.key();
        if (!ev.value().is_array()) {
            auto d = diag(HookDiagnosticSeverity::Warning, "BAD_EVENT_GROUPS",
                          "hooks." + event_name + " must be an array", source,
                          std::string{}, event_name);
            snapshot.diagnostics.push_back(d);
            snapshot.sources.back().diagnostics.push_back(std::move(d));
            continue;
        }

        int matcher_group_index = 0;
        for (const auto& group : ev.value()) {
            ++matcher_group_index;
            if (!group.is_object()) {
                auto d = diag(HookDiagnosticSeverity::Warning, "BAD_MATCHER_GROUP",
                              "hook matcher group must be an object", source,
                              std::string{}, event_name);
                snapshot.diagnostics.push_back(d);
                snapshot.sources.back().diagnostics.push_back(std::move(d));
                continue;
            }

            std::string matcher;
            if (group.contains("matcher") && group["matcher"].is_string()) {
                matcher = group["matcher"].get<std::string>();
            }

            if (!group.contains("hooks") || !group["hooks"].is_array()) {
                auto d = diag(HookDiagnosticSeverity::Warning, "BAD_HANDLERS",
                              "matcher group hooks must be an array", source,
                              std::string{}, event_name);
                snapshot.diagnostics.push_back(d);
                snapshot.sources.back().diagnostics.push_back(std::move(d));
                continue;
            }

            int handler_index = 0;
            for (const auto& handler : group["hooks"]) {
                ++handler_index;
                NormalizedHook hook;
                hook.source_id = source.id;
                hook.source_scope = source.scope;
                hook.source_format = source.format;
                hook.source_path = source.path;
                hook.event_name = event_name;
                hook.matcher = matcher;
                hook.matcher_group_index = matcher_group_index;
                hook.handler_index = handler_index;
                hook.managed = source.managed || source.scope == HookSourceScope::Managed;
                hook.command.timeout_seconds = kDefaultCodexHookTimeoutSeconds;
                hook.command.config_location =
                    source.path + ":" + event_name + "#" +
                    std::to_string(matcher_group_index) + "." +
                    std::to_string(handler_index);
                hook.id = make_stable_hook_id(source, event_name,
                                              matcher_group_index, handler_index);

                if (!handler.is_object()) {
                    hook.kind = HookHandlerKind::Unknown;
                    hook.skipped = true;
                    hook.skip_reason = "hook handler must be an object";
                    hook.diagnostics.push_back(diag(
                        HookDiagnosticSeverity::Warning, "BAD_HANDLER",
                        hook.skip_reason, source, hook.id, event_name));
                    finalize_hook(hook);
                    snapshot.diagnostics.insert(snapshot.diagnostics.end(),
                                                hook.diagnostics.begin(),
                                                hook.diagnostics.end());
                    snapshot.hooks.push_back(std::move(hook));
                    continue;
                }

                std::string type = "command";
                if (handler.contains("type") && handler["type"].is_string()) {
                    type = lower_ascii(handler["type"].get<std::string>());
                }

                if (type == "prompt") {
                    hook.kind = HookHandlerKind::UnsupportedPrompt;
                    hook.skipped = true;
                    hook.skip_reason = "prompt hook handlers are not supported";
                } else if (type == "agent") {
                    hook.kind = HookHandlerKind::UnsupportedAgent;
                    hook.skipped = true;
                    hook.skip_reason = "agent hook handlers are not supported";
                } else if (type != "command") {
                    hook.kind = HookHandlerKind::Unknown;
                    hook.skipped = true;
                    hook.skip_reason = "unsupported hook handler type: " + type;
                } else {
                    hook.kind = HookHandlerKind::Command;
                    if (handler.contains("command") && handler["command"].is_string()) {
                        hook.command.command = handler["command"].get<std::string>();
                    }
                    if (handler.contains("commandWindows") &&
                        handler["commandWindows"].is_string()) {
                        hook.command.command_windows =
                            handler["commandWindows"].get<std::string>();
                    }
                    if (handler.contains("command_windows") &&
                        handler["command_windows"].is_string()) {
                        hook.command.command_windows =
                            handler["command_windows"].get<std::string>();
                    }
                    if (handler.contains("timeout") &&
                        handler["timeout"].is_number_integer()) {
                        int timeout = handler["timeout"].get<int>();
                        hook.command.timeout_seconds =
                            timeout > 0 ? timeout : kDefaultCodexHookTimeoutSeconds;
                    }
                    if (handler.contains("statusMessage") &&
                        handler["statusMessage"].is_string()) {
                        hook.command.status_message =
                            handler["statusMessage"].get<std::string>();
                    }
                    if (handler.contains("async") && handler["async"].is_boolean()) {
                        hook.command.async = handler["async"].get<bool>();
                    }
                    if (hook.command.async) {
                        hook.skipped = true;
                        hook.skip_reason = "async Codex command hooks are not supported";
                    } else if (trim_ascii(hook.command.command).empty() &&
                               trim_ascii(hook.command.command_windows).empty()) {
                        hook.skipped = true;
                        hook.skip_reason = "command hook must define command";
                    }
                }

                if (hook.skipped) {
                    hook.diagnostics.push_back(diag(
                        HookDiagnosticSeverity::Warning, "SKIPPED_HANDLER",
                        hook.skip_reason, source, hook.id, event_name));
                }
                finalize_hook(hook);
                snapshot.diagnostics.insert(snapshot.diagnostics.end(),
                                            hook.diagnostics.begin(),
                                            hook.diagnostics.end());
                snapshot.hooks.push_back(std::move(hook));
            }
        }
    }
    return snapshot;
}

HookRegistrySnapshot parse_legacy_hooks_json_source(const nlohmann::json& root,
                                                    HookSource source,
                                                    bool auto_trust_user_legacy_hooks) {
    HookRegistrySnapshot snapshot;
    source.format = HookSourceFormat::AceCodeLegacyJson;
    source.id = make_hook_source_id(source.scope, source.format, source.path);
    snapshot.sources.push_back(source);

    std::string error;
    HookConfig cfg = parse_hook_config_json(root, &error);
    if (!error.empty()) {
        auto d = diag(HookDiagnosticSeverity::Error, "BAD_LEGACY_CONFIG",
                      error, source);
        snapshot.sources.back().diagnostics.push_back(d);
        snapshot.diagnostics.push_back(std::move(d));
        return snapshot;
    }
    if (!cfg.enabled) {
        auto d = diag(HookDiagnosticSeverity::Info, "LEGACY_DISABLED",
                      "legacy ACECode hooks are disabled by config", source);
        snapshot.sources.back().diagnostics.push_back(d);
        snapshot.diagnostics.push_back(std::move(d));
        return snapshot;
    }

    for (const auto& [event_name, hooks] : cfg.events) {
        int handler_index = 0;
        for (const auto& legacy : hooks) {
            ++handler_index;
            NormalizedHook hook;
            hook.source_id = source.id;
            hook.source_scope = source.scope;
            hook.source_format = source.format;
            hook.source_path = source.path;
            hook.event_name = event_name;
            hook.matcher = "*";
            hook.matcher_group_index = 1;
            hook.handler_index = handler_index;
            hook.kind = HookHandlerKind::Command;
            hook.legacy_direct = true;
            hook.legacy.definition = legacy;
            hook.managed = source.managed || source.scope == HookSourceScope::Managed;
            hook.id = hook_id_from_config_or_fallback(
                source, legacy.id, event_name, 1, handler_index);
            finalize_hook(hook);
            if (source_is_auto_trusted_legacy_user(hook,
                                                   auto_trust_user_legacy_hooks)) {
                hook.trust_status = HookTrustStatus::Trusted;
            }
            snapshot.hooks.push_back(std::move(hook));
        }
    }
    return snapshot;
}

HookRegistrySnapshot parse_hook_source_json(const nlohmann::json& root,
                                            HookSource source,
                                            bool auto_trust_user_legacy_hooks) {
    if (source.format == HookSourceFormat::AceCodeLegacyJson ||
        (source.format == HookSourceFormat::Unknown &&
         root.is_object() && root.contains("events"))) {
        if (source.scope == HookSourceScope::UserGlobal &&
            source.path.find(".acecode") != std::string::npos) {
            source.scope = HookSourceScope::Legacy;
        }
        return parse_legacy_hooks_json_source(
            root, std::move(source), auto_trust_user_legacy_hooks);
    }
    return parse_codex_hooks_json_source(root, std::move(source));
}

std::string default_codex_home_dir() {
    std::string home = home_dir_for_codex();
    if (home.empty()) return {};
    return path_to_utf8(path_from_utf8(home) / ".codex");
}

std::string default_hook_trust_state_path() {
    return path_to_utf8(path_from_utf8(get_acecode_dir()) / "hooks_state.json");
}

HookTrustStore parse_hook_trust_store_json(const nlohmann::json& root,
                                           std::string* error) {
    HookTrustStore store;
    if (error) error->clear();
    if (!root.is_object()) {
        if (error) *error = "hook trust state root must be an object";
        return store;
    }
    if (root.contains("version") && root["version"].is_number_integer()) {
        store.version = root["version"].get<int>();
    }
    if (root.contains("trusted") && root["trusted"].is_array()) {
        for (const auto& item : root["trusted"]) {
            if (!item.is_object()) continue;
            HookTrustRecord record;
            record.source_id = item.value("source_id", std::string{});
            record.hook_id = item.value("hook_id", std::string{});
            record.definition_hash = item.value("definition_hash", std::string{});
            if (!record.source_id.empty() &&
                !record.hook_id.empty() &&
                !record.definition_hash.empty()) {
                store.trusted.push_back(std::move(record));
            }
        }
    }
    if (root.contains("disabled") && root["disabled"].is_array()) {
        for (const auto& item : root["disabled"]) {
            if (!item.is_object()) continue;
            HookDisabledRecord record;
            record.source_id = item.value("source_id", std::string{});
            record.hook_id = item.value("hook_id", std::string{});
            if (!record.source_id.empty() && !record.hook_id.empty()) {
                store.disabled.push_back(std::move(record));
            }
        }
    }
    return store;
}

nlohmann::json hook_trust_store_to_json(const HookTrustStore& store) {
    nlohmann::json root = nlohmann::json::object();
    root["version"] = store.version;
    root["trusted"] = nlohmann::json::array();
    for (const auto& record : store.trusted) {
        root["trusted"].push_back({
            {"source_id", record.source_id},
            {"hook_id", record.hook_id},
            {"definition_hash", record.definition_hash},
        });
    }
    root["disabled"] = nlohmann::json::array();
    for (const auto& record : store.disabled) {
        root["disabled"].push_back({
            {"source_id", record.source_id},
            {"hook_id", record.hook_id},
        });
    }
    return root;
}

HookTrustStore load_hook_trust_store_from_path(const std::string& path,
                                               std::string* error) {
    if (error) error->clear();
    std::error_code ec;
    if (!fs::is_regular_file(path_from_utf8(path), ec)) return HookTrustStore{};
    std::string parse_error;
    auto root = load_json_file(path, &parse_error);
    if (!root.has_value()) {
        if (error) *error = "failed to parse hook trust state: " + parse_error;
        return HookTrustStore{};
    }
    return parse_hook_trust_store_json(*root, error);
}

bool save_hook_trust_store_to_path(const HookTrustStore& store,
                                   const std::string& path,
                                   std::string* error) {
    if (error) error->clear();
    if (path.empty()) {
        if (error) *error = "hook trust state path is empty";
        return false;
    }
    if (!atomic_write_file(path, hook_trust_store_to_json(store).dump(2), true)) {
        if (error) *error = "failed to write hook trust state: " + path;
        return false;
    }
    return true;
}

bool hook_is_trusted(const HookTrustStore& store, const NormalizedHook& hook) {
    for (const auto& record : store.trusted) {
        if (record.source_id == hook.source_id &&
            record.hook_id == hook.id &&
            record.definition_hash == hook.definition_hash) {
            return true;
        }
    }
    return false;
}

bool hook_is_disabled(const HookTrustStore& store, const NormalizedHook& hook) {
    for (const auto& record : store.disabled) {
        if (record.source_id == hook.source_id && record.hook_id == hook.id) {
            return true;
        }
    }
    return false;
}

void trust_hook_definition(HookTrustStore& store, const NormalizedHook& hook) {
    auto it = std::find_if(store.trusted.begin(), store.trusted.end(),
        [&](const HookTrustRecord& record) {
            return record.source_id == hook.source_id &&
                record.hook_id == hook.id &&
                record.definition_hash == hook.definition_hash;
        });
    if (it != store.trusted.end()) return;
    store.trusted.push_back({hook.source_id, hook.id, hook.definition_hash});
}

void set_hook_disabled(HookTrustStore& store,
                       const NormalizedHook& hook,
                       bool disabled) {
    auto pred = [&](const HookDisabledRecord& record) {
        return record.source_id == hook.source_id && record.hook_id == hook.id;
    };
    auto it = std::remove_if(store.disabled.begin(), store.disabled.end(), pred);
    store.disabled.erase(it, store.disabled.end());
    if (disabled && !hook.managed) {
        store.disabled.push_back({hook.source_id, hook.id});
    }
}

void apply_hook_trust_state(HookRegistrySnapshot& snapshot,
                            const HookTrustStore& store,
                            bool auto_trust_user_legacy_hooks) {
    for (auto& hook : snapshot.hooks) {
        if (hook.skipped) {
            hook.trust_status = HookTrustStatus::SkippedUnsupported;
            continue;
        }
        if (hook.managed) {
            hook.trust_status = HookTrustStatus::ManagedTrusted;
            continue;
        }
        if (hook_is_disabled(store, hook)) {
            hook.trust_status = HookTrustStatus::Disabled;
            continue;
        }
        if (source_is_auto_trusted_legacy_user(hook, auto_trust_user_legacy_hooks) ||
            hook_is_trusted(store, hook)) {
            hook.trust_status = HookTrustStatus::Trusted;
            continue;
        }
        hook.trust_status = HookTrustStatus::PendingReview;
    }
}

HookRegistrySnapshot load_hook_registry(const HookLoadOptions& options,
                                        const HookTrustStore* trust_store) {
    HookRegistrySnapshot snapshot;
    snapshot.feature_enabled = options.feature_enabled;
    if (!options.feature_enabled) {
        snapshot.diagnostics.push_back({
            HookDiagnosticSeverity::Info,
            "HOOKS_DISABLED",
            "hooks are disabled by feature flag",
            {},
            {},
            {},
        });
        return snapshot;
    }

    const std::string ace_home = options.acecode_home.empty()
        ? get_acecode_dir()
        : options.acecode_home;
    const std::string codex_home = options.codex_home.empty()
        ? default_codex_home_dir()
        : options.codex_home;

    struct Candidate {
        HookSource source;
        bool project_local = false;
    };
    std::vector<Candidate> candidates;
    candidates.push_back({
        make_source(HookSourceScope::Legacy, HookSourceFormat::Unknown,
                    path_to_utf8(path_from_utf8(ace_home) / "hooks.json")),
        false,
    });
    if (!codex_home.empty()) {
        candidates.push_back({
            make_source(HookSourceScope::UserGlobal, HookSourceFormat::CodexJson,
                        path_to_utf8(path_from_utf8(codex_home) / "hooks.json")),
            false,
        });
    }
    if (options.include_project_sources && !options.cwd.empty()) {
        candidates.push_back({
            make_source(HookSourceScope::ProjectLocal, HookSourceFormat::Unknown,
                        path_to_utf8(path_from_utf8(options.cwd) / ".acecode" / "hooks.json"),
                        false, options.project_trusted),
            true,
        });
        candidates.push_back({
            make_source(HookSourceScope::ProjectLocal, HookSourceFormat::CodexJson,
                        path_to_utf8(path_from_utf8(options.cwd) / ".codex" / "hooks.json"),
                        false, options.project_trusted),
            true,
        });
    }

    for (const auto& candidate : candidates) {
        if (candidate.project_local && !options.project_trusted) {
            snapshot.diagnostics.push_back(diag(
                HookDiagnosticSeverity::Info, "PROJECT_HOOKS_UNTRUSTED",
                "project-local hook source skipped because the workspace is not trusted",
                candidate.source));
            continue;
        }

        std::error_code ec;
        if (!fs::is_regular_file(path_from_utf8(candidate.source.path), ec)) {
            snapshot.diagnostics.push_back(diag(
                HookDiagnosticSeverity::Info, "HOOK_SOURCE_MISSING",
                "hook source not found: " + candidate.source.path,
                candidate.source));
            continue;
        }

        std::string error;
        auto root = load_json_file(candidate.source.path, &error);
        if (!root.has_value()) {
            HookSource source = candidate.source;
            auto d = diag(HookDiagnosticSeverity::Error, "HOOK_SOURCE_PARSE_FAILED",
                          "failed to parse hook source: " + error, source);
            source.diagnostics.push_back(d);
            snapshot.sources.push_back(std::move(source));
            snapshot.diagnostics.push_back(std::move(d));
            continue;
        }

        merge_snapshot(snapshot, parse_hook_source_json(
            *root, candidate.source, options.auto_trust_user_legacy_hooks));
    }

    HookTrustStore empty_store;
    apply_hook_trust_state(snapshot,
                           trust_store ? *trust_store : empty_store,
                           options.auto_trust_user_legacy_hooks);
    return snapshot;
}

nlohmann::json hook_source_to_json(const HookSource& source) {
    nlohmann::json j = nlohmann::json::object();
    j["id"] = source.id;
    j["scope"] = hook_source_scope_name(source.scope);
    j["format"] = hook_source_format_name(source.format);
    j["path"] = source.path;
    j["label"] = source.label;
    j["managed"] = source.managed;
    j["project_trusted"] = source.project_trusted;
    j["diagnostics"] = nlohmann::json::array();
    for (const auto& d : source.diagnostics) {
        j["diagnostics"].push_back({
            {"severity", hook_diagnostic_severity_name(d.severity)},
            {"code", d.code},
            {"message", d.message},
            {"source_id", d.source_id},
            {"hook_id", d.hook_id},
            {"event_name", d.event_name},
        });
    }
    return j;
}

nlohmann::json normalized_hook_to_json(const NormalizedHook& hook) {
    nlohmann::json j = nlohmann::json::object();
    j["id"] = hook.id;
    j["source_id"] = hook.source_id;
    j["source_scope"] = hook_source_scope_name(hook.source_scope);
    j["source_format"] = hook_source_format_name(hook.source_format);
    j["source_path"] = hook.source_path;
    j["event_name"] = hook.event_name;
    j["matcher"] = hook.matcher;
    j["matcher_group_index"] = hook.matcher_group_index;
    j["handler_index"] = hook.handler_index;
    j["kind"] = hook_handler_kind_name(hook.kind);
    j["legacy_direct"] = hook.legacy_direct;
    j["managed"] = hook.managed;
    j["skipped"] = hook.skipped;
    j["skip_reason"] = hook.skip_reason;
    j["definition_hash"] = hook.definition_hash;
    j["last_run_status"] = nullptr;
    j["trust_status"] = hook_trust_status_name(hook.trust_status);
    j["disabled"] = hook.trust_status == HookTrustStatus::Disabled;
    j["pending_review"] = hook.trust_status == HookTrustStatus::PendingReview;
    j["trusted"] = hook.trust_status == HookTrustStatus::Trusted ||
        hook.trust_status == HookTrustStatus::ManagedTrusted;
    if (hook.legacy_direct) {
        const HookDefinition& h = hook.legacy.definition;
        j["command"] = h.command.command;
        j["args"] = h.command.args;
        j["mode"] = h.mode == HookMode::Async ? "async" : "sync";
        j["timeout_ms"] = h.timeout_ms;
    } else {
        j["command"] = hook.command.command;
        j["command_windows"] = hook.command.command_windows;
        j["timeout_seconds"] = hook.command.timeout_seconds;
        j["status_message"] = hook.command.status_message;
        j["async"] = hook.command.async;
        j["config_location"] = hook.command.config_location;
    }
    j["diagnostics"] = nlohmann::json::array();
    for (const auto& d : hook.diagnostics) {
        j["diagnostics"].push_back({
            {"severity", hook_diagnostic_severity_name(d.severity)},
            {"code", d.code},
            {"message", d.message},
            {"source_id", d.source_id},
            {"hook_id", d.hook_id},
            {"event_name", d.event_name},
        });
    }
    return j;
}

nlohmann::json hook_registry_snapshot_to_json(const HookRegistrySnapshot& snapshot) {
    nlohmann::json j = nlohmann::json::object();
    j["feature_enabled"] = snapshot.feature_enabled;
    j["sources"] = nlohmann::json::array();
    for (const auto& source : snapshot.sources) {
        j["sources"].push_back(hook_source_to_json(source));
    }
    j["hooks"] = nlohmann::json::array();
    for (const auto& hook : snapshot.hooks) {
        j["hooks"].push_back(normalized_hook_to_json(hook));
    }
    j["diagnostics"] = nlohmann::json::array();
    for (const auto& d : snapshot.diagnostics) {
        j["diagnostics"].push_back({
            {"severity", hook_diagnostic_severity_name(d.severity)},
            {"code", d.code},
            {"message", d.message},
            {"source_id", d.source_id},
            {"hook_id", d.hook_id},
            {"event_name", d.event_name},
        });
    }
    return j;
}

} // namespace acecode
