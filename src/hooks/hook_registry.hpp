#pragma once

#include "hook_config.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace acecode {

enum class HookSourceScope {
    UserGlobal = 0,
    ProjectLocal,
    Managed,
    Plugin,
    Legacy,
    Inline,
};

enum class HookSourceFormat {
    Unknown = 0,
    AceCodeLegacyJson,
    CodexJson,
    CodexInline,
};

enum class HookHandlerKind {
    Command = 0,
    UnsupportedPrompt,
    UnsupportedAgent,
    Unknown,
};

enum class HookTrustStatus {
    Trusted = 0,
    PendingReview,
    Disabled,
    ManagedTrusted,
    SkippedUnsupported,
};

enum class HookDiagnosticSeverity {
    Info = 0,
    Warning,
    Error,
};

struct HookDiagnostic {
    HookDiagnosticSeverity severity = HookDiagnosticSeverity::Warning;
    std::string code;
    std::string message;
    std::string source_id;
    std::string hook_id;
    std::string event_name;
};

struct HookSource {
    std::string id;
    HookSourceScope scope = HookSourceScope::UserGlobal;
    HookSourceFormat format = HookSourceFormat::Unknown;
    std::string path;
    std::string label;
    bool managed = false;
    bool project_trusted = true;
    std::vector<HookDiagnostic> diagnostics;
};

struct HookCommandHandler {
    std::string command;
    std::string command_windows;
    int timeout_seconds = 600;
    std::string status_message;
    bool async = false;
    std::string config_location;
};

struct HookLegacyHandler {
    HookDefinition definition;
};

struct NormalizedHook {
    std::string id;
    std::string source_id;
    HookSourceScope source_scope = HookSourceScope::UserGlobal;
    HookSourceFormat source_format = HookSourceFormat::Unknown;
    std::string source_path;
    std::string event_name;
    std::string matcher;
    int matcher_group_index = 0;
    int handler_index = 0;
    HookHandlerKind kind = HookHandlerKind::Command;
    HookCommandHandler command;
    HookLegacyHandler legacy;
    bool legacy_direct = false;
    bool managed = false;
    bool skipped = false;
    std::string skip_reason;
    std::string definition_hash;
    HookTrustStatus trust_status = HookTrustStatus::PendingReview;
    std::vector<HookDiagnostic> diagnostics;
};

struct HookRegistrySnapshot {
    bool feature_enabled = true;
    std::vector<HookSource> sources;
    std::vector<NormalizedHook> hooks;
    std::vector<HookDiagnostic> diagnostics;
};

struct HookLoadOptions {
    bool feature_enabled = true;
    std::string cwd;
    bool project_trusted = true;
    std::string acecode_home;
    std::string codex_home;
    bool include_project_sources = true;
    bool auto_trust_user_legacy_hooks = true;
};

struct HookTrustRecord {
    std::string source_id;
    std::string hook_id;
    std::string definition_hash;
};

struct HookDisabledRecord {
    std::string source_id;
    std::string hook_id;
};

struct HookTrustStore {
    int version = 1;
    std::vector<HookTrustRecord> trusted;
    std::vector<HookDisabledRecord> disabled;
};

std::string hook_source_scope_name(HookSourceScope scope);
std::string hook_source_format_name(HookSourceFormat format);
std::string hook_handler_kind_name(HookHandlerKind kind);
std::string hook_trust_status_name(HookTrustStatus status);
std::string hook_diagnostic_severity_name(HookDiagnosticSeverity severity);

std::string make_hook_source_id(HookSourceScope scope,
                                HookSourceFormat format,
                                const std::string& identity);
std::string make_stable_hook_id(const HookSource& source,
                                const std::string& event_name,
                                int matcher_group_index,
                                int handler_index);
std::string stable_hook_definition_hash(const NormalizedHook& hook);

HookRegistrySnapshot parse_codex_hooks_json_source(const nlohmann::json& root,
                                                   HookSource source);
HookRegistrySnapshot parse_legacy_hooks_json_source(const nlohmann::json& root,
                                                    HookSource source,
                                                    bool auto_trust_user_legacy_hooks);
HookRegistrySnapshot parse_hook_source_json(const nlohmann::json& root,
                                            HookSource source,
                                            bool auto_trust_user_legacy_hooks);

std::string default_codex_home_dir();
std::string default_hook_trust_state_path();

HookTrustStore parse_hook_trust_store_json(const nlohmann::json& root,
                                           std::string* error = nullptr);
nlohmann::json hook_trust_store_to_json(const HookTrustStore& store);
HookTrustStore load_hook_trust_store_from_path(const std::string& path,
                                               std::string* error = nullptr);
bool save_hook_trust_store_to_path(const HookTrustStore& store,
                                   const std::string& path,
                                   std::string* error = nullptr);

bool hook_is_trusted(const HookTrustStore& store, const NormalizedHook& hook);
bool hook_is_disabled(const HookTrustStore& store, const NormalizedHook& hook);
void trust_hook_definition(HookTrustStore& store, const NormalizedHook& hook);
void set_hook_disabled(HookTrustStore& store,
                       const NormalizedHook& hook,
                       bool disabled);
void apply_hook_trust_state(HookRegistrySnapshot& snapshot,
                            const HookTrustStore& store,
                            bool auto_trust_user_legacy_hooks);

HookRegistrySnapshot load_hook_registry(const HookLoadOptions& options,
                                        const HookTrustStore* trust_store = nullptr);

nlohmann::json hook_source_to_json(const HookSource& source);
nlohmann::json normalized_hook_to_json(const NormalizedHook& hook);
nlohmann::json hook_registry_snapshot_to_json(const HookRegistrySnapshot& snapshot);

} // namespace acecode
