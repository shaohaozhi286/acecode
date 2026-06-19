#include <gtest/gtest.h>

#include "hooks/hook_registry.hpp"
#include "utils/utf8_path.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

struct TempTree {
    fs::path root;

    TempTree() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root = fs::temp_directory_path() /
               ("acecode-hook-registry-test-" + std::to_string(stamp));
        fs::create_directories(root);
    }

    ~TempTree() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

void write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    ofs << text;
}

acecode::HookSource source_for(const std::string& path,
                               acecode::HookSourceScope scope = acecode::HookSourceScope::UserGlobal,
                               acecode::HookSourceFormat format = acecode::HookSourceFormat::CodexJson) {
    acecode::HookSource source;
    source.scope = scope;
    source.format = format;
    source.path = path;
    source.id = acecode::make_hook_source_id(scope, format, path);
    return source;
}

} // namespace

TEST(HookRegistry, StableIdsAndHashesIgnoreJsonKeyOrder) {
    auto a = nlohmann::json::parse(R"({
        "hooks": {
            "PreToolUse": [
                {
                    "matcher": "Bash",
                    "hooks": [
                        {
                            "type": "command",
                            "command": "node hook.js",
                            "commandWindows": "node hook-win.js",
                            "timeout": 10,
                            "statusMessage": "checking"
                        }
                    ]
                }
            ]
        }
    })");
    auto b = nlohmann::json::parse(R"({
        "hooks": {
            "PreToolUse": [
                {
                    "hooks": [
                        {
                            "statusMessage": "checking",
                            "timeout": 10,
                            "commandWindows": "node hook-win.js",
                            "command": "node hook.js",
                            "type": "command"
                        }
                    ],
                    "matcher": "Bash"
                }
            ]
        }
    })");

    auto source = source_for("C:/repo/.codex/hooks.json");
    auto ra = acecode::parse_codex_hooks_json_source(a, source);
    auto rb = acecode::parse_codex_hooks_json_source(b, source);

    ASSERT_EQ(ra.hooks.size(), 1u);
    ASSERT_EQ(rb.hooks.size(), 1u);
    EXPECT_EQ(ra.hooks[0].id, source.id + "::PreToolUse#1.1");
    EXPECT_EQ(ra.hooks[0].id, rb.hooks[0].id);
    EXPECT_EQ(ra.hooks[0].definition_hash, rb.hooks[0].definition_hash);
}

TEST(HookRegistry, ParsesCodexCommandFieldsAndDefaultTimeout) {
    auto j = nlohmann::json::parse(R"({
        "hooks": {
            "SessionStart": [
                {
                    "hooks": [
                        {
                            "type": "command",
                            "command": "python hook.py",
                            "command_windows": "py hook.py",
                            "statusMessage": "Loading hooks"
                        }
                    ]
                }
            ]
        }
    })");

    auto registry = acecode::parse_codex_hooks_json_source(
        j, source_for("/repo/.codex/hooks.json"));
    ASSERT_EQ(registry.hooks.size(), 1u);
    const auto& hook = registry.hooks[0];
    EXPECT_EQ(hook.event_name, "SessionStart");
    EXPECT_EQ(hook.matcher, "");
    EXPECT_EQ(hook.command.command, "python hook.py");
    EXPECT_EQ(hook.command.command_windows, "py hook.py");
    EXPECT_EQ(hook.command.timeout_seconds, 600);
    EXPECT_EQ(hook.command.status_message, "Loading hooks");
    EXPECT_FALSE(hook.skipped);
}

TEST(HookRegistry, ParsesUnsupportedAndAsyncHandlersAsSkipped) {
    auto j = nlohmann::json::parse(R"({
        "hooks": {
            "Stop": [
                {
                    "matcher": "*",
                    "hooks": [
                        {"type": "prompt", "prompt": "x"},
                        {"type": "agent", "name": "x"},
                        {"type": "command", "command": "echo async", "async": true},
                        {"type": "weird"}
                    ]
                }
            ]
        }
    })");

    auto registry = acecode::parse_codex_hooks_json_source(
        j, source_for("/repo/.codex/hooks.json"));
    ASSERT_EQ(registry.hooks.size(), 4u);
    EXPECT_EQ(registry.hooks[0].kind, acecode::HookHandlerKind::UnsupportedPrompt);
    EXPECT_EQ(registry.hooks[1].kind, acecode::HookHandlerKind::UnsupportedAgent);
    EXPECT_EQ(registry.hooks[2].kind, acecode::HookHandlerKind::Command);
    EXPECT_EQ(registry.hooks[3].kind, acecode::HookHandlerKind::Unknown);
    for (const auto& hook : registry.hooks) {
        EXPECT_TRUE(hook.skipped);
        EXPECT_FALSE(hook.skip_reason.empty());
    }
    EXPECT_GE(registry.diagnostics.size(), 4u);
}

TEST(HookRegistry, KeepsLegacyDirectCommandSemantics) {
    auto j = nlohmann::json::parse(R"({
        "enabled": true,
        "events": {
            "startup.models_loaded": [
                {
                    "id": "legacy-startup",
                    "mode": "async",
                    "command": "python",
                    "args": ["hook.py"],
                    "timeout_ms": 1234
                }
            ]
        }
    })");

    auto registry = acecode::parse_hook_source_json(
        j,
        source_for("C:/Users/me/.acecode/hooks.json",
                   acecode::HookSourceScope::Legacy,
                   acecode::HookSourceFormat::Unknown),
        true);

    ASSERT_EQ(registry.hooks.size(), 1u);
    const auto& hook = registry.hooks[0];
    EXPECT_TRUE(hook.legacy_direct);
    EXPECT_EQ(hook.id.find("legacy-startup"), hook.id.size() - std::string("legacy-startup").size());
    EXPECT_EQ(hook.legacy.definition.command.command, "python");
    ASSERT_EQ(hook.legacy.definition.command.args.size(), 1u);
    EXPECT_EQ(hook.legacy.definition.command.args[0], "hook.py");
    EXPECT_EQ(hook.legacy.definition.mode, acecode::HookMode::Async);
    EXPECT_EQ(hook.legacy.definition.timeout_ms, 1234);
    EXPECT_EQ(hook.trust_status, acecode::HookTrustStatus::Trusted);
}

TEST(HookRegistry, DisabledLegacyConfigStillSurfacesHooksAsDisabled) {
    auto j = nlohmann::json::parse(R"({
        "enabled": false,
        "events": {
            "startup.before_model_load": [
                {
                    "id": "legacy-startup",
                    "command": "node",
                    "args": ["hook.js"]
                }
            ]
        }
    })");

    auto registry = acecode::parse_hook_source_json(
        j,
        source_for("C:/Users/me/.acecode/hooks.json",
                   acecode::HookSourceScope::Legacy,
                   acecode::HookSourceFormat::Unknown),
        true);
    acecode::HookTrustStore store;
    acecode::apply_hook_trust_state(registry, store, true);

    ASSERT_EQ(registry.hooks.size(), 1u);
    EXPECT_EQ(registry.hooks[0].trust_status, acecode::HookTrustStatus::Disabled);
    bool saw_disabled_diag = false;
    for (const auto& d : registry.diagnostics) {
        if (d.code == "LEGACY_DISABLED") saw_disabled_diag = true;
    }
    EXPECT_TRUE(saw_disabled_diag);
}

TEST(HookRegistry, SourceDiscoveryMergesGlobalAndTrustedProjectSources) {
    TempTree tmp;
    const fs::path ace_home = tmp.root / "ace-home";
    const fs::path codex_home = tmp.root / "codex-home";
    const fs::path project = tmp.root / "project";

    write_text(ace_home / "hooks.json", R"({
        "enabled": true,
        "events": {
            "startup.before_model_load": [
                {"id": "legacy", "command": "legacy-hook"}
            ]
        }
    })");
    write_text(codex_home / "hooks.json", R"({
        "hooks": {
            "SessionStart": [
                {"hooks": [{"type": "command", "command": "global-codex"}]}
            ]
        }
    })");
    write_text(project / ".acecode" / "hooks.json", R"({
        "hooks": {
            "PreToolUse": [
                {"matcher": "Bash", "hooks": [{"type": "command", "command": "project-ace"}]}
            ]
        }
    })");
    write_text(project / ".codex" / "hooks.json", R"({
        "hooks": {
            "Stop": [
                {"hooks": [{"type": "command", "command": "project-codex"}]}
            ]
        }
    })");

    acecode::HookLoadOptions opts;
    opts.acecode_home = acecode::path_to_utf8(ace_home);
    opts.codex_home = acecode::path_to_utf8(codex_home);
    opts.cwd = acecode::path_to_utf8(project);
    opts.project_trusted = true;
    auto trusted = acecode::load_hook_registry(opts);
    EXPECT_EQ(trusted.hooks.size(), 4u);
    EXPECT_EQ(trusted.sources.size(), 4u);

    opts.project_trusted = false;
    auto untrusted = acecode::load_hook_registry(opts);
    EXPECT_EQ(untrusted.hooks.size(), 2u);
    bool saw_skip = false;
    for (const auto& d : untrusted.diagnostics) {
        if (d.code == "PROJECT_HOOKS_UNTRUSTED") saw_skip = true;
    }
    EXPECT_TRUE(saw_skip);
}

TEST(HookRegistry, SourceDiscoveryReportsMalformedSources) {
    TempTree tmp;
    const fs::path ace_home = tmp.root / "ace-home";
    write_text(ace_home / "hooks.json", "{not-json");

    acecode::HookLoadOptions opts;
    opts.acecode_home = acecode::path_to_utf8(ace_home);
    opts.codex_home = acecode::path_to_utf8(tmp.root / "missing-codex");
    opts.cwd = acecode::path_to_utf8(tmp.root / "project");
    auto registry = acecode::load_hook_registry(opts);

    EXPECT_TRUE(registry.hooks.empty());
    bool saw_parse = false;
    bool saw_missing = false;
    for (const auto& d : registry.diagnostics) {
        if (d.code == "HOOK_SOURCE_PARSE_FAILED") saw_parse = true;
        if (d.code == "HOOK_SOURCE_MISSING") saw_missing = true;
    }
    EXPECT_TRUE(saw_parse);
    EXPECT_TRUE(saw_missing);
}

TEST(HookRegistry, TrustStoreHandlesPendingTrustedChangedDisabledAndManaged) {
    auto j = nlohmann::json::parse(R"({
        "hooks": {
            "PreToolUse": [
                {"matcher": "Bash", "hooks": [{"type": "command", "command": "echo one"}]}
            ]
        }
    })");
    auto source = source_for("/repo/.codex/hooks.json");
    auto registry = acecode::parse_codex_hooks_json_source(j, source);
    ASSERT_EQ(registry.hooks.size(), 1u);

    acecode::HookTrustStore store;
    acecode::apply_hook_trust_state(registry, store, true);
    EXPECT_EQ(registry.hooks[0].trust_status, acecode::HookTrustStatus::PendingReview);

    acecode::trust_hook_definition(store, registry.hooks[0]);
    acecode::apply_hook_trust_state(registry, store, true);
    EXPECT_EQ(registry.hooks[0].trust_status, acecode::HookTrustStatus::Trusted);

    auto changed_json = nlohmann::json::parse(R"({
        "hooks": {
            "PreToolUse": [
                {"matcher": "Bash", "hooks": [{"type": "command", "command": "echo two"}]}
            ]
        }
    })");
    auto changed = acecode::parse_codex_hooks_json_source(changed_json, source);
    acecode::apply_hook_trust_state(changed, store, true);
    ASSERT_EQ(changed.hooks.size(), 1u);
    EXPECT_EQ(changed.hooks[0].trust_status, acecode::HookTrustStatus::PendingReview);

    acecode::set_hook_disabled(store, registry.hooks[0], true);
    acecode::apply_hook_trust_state(registry, store, true);
    EXPECT_EQ(registry.hooks[0].trust_status, acecode::HookTrustStatus::Disabled);

    auto managed_source = source_for("/managed/hooks.json",
                                     acecode::HookSourceScope::Managed,
                                     acecode::HookSourceFormat::CodexJson);
    managed_source.managed = true;
    auto managed = acecode::parse_codex_hooks_json_source(j, managed_source);
    acecode::apply_hook_trust_state(managed, store, true);
    ASSERT_EQ(managed.hooks.size(), 1u);
    EXPECT_EQ(managed.hooks[0].trust_status, acecode::HookTrustStatus::ManagedTrusted);
}

TEST(HookRegistry, TrustStorePersistsAtomicallyEnoughForFailureRollback) {
    TempTree tmp;
    const auto path = tmp.root / "hooks_state.json";

    acecode::HookTrustStore store;
    store.trusted.push_back({"source", "hook", "hash-a"});
    std::string error;
    ASSERT_TRUE(acecode::save_hook_trust_store_to_path(
        store, acecode::path_to_utf8(path), &error)) << error;

    auto loaded = acecode::load_hook_trust_store_from_path(acecode::path_to_utf8(path), &error);
    ASSERT_EQ(loaded.trusted.size(), 1u);
    EXPECT_EQ(loaded.trusted[0].definition_hash, "hash-a");

    fs::path bad_target = tmp.root / "bad-dir";
    fs::create_directories(bad_target);
    acecode::HookTrustStore changed;
    changed.trusted.push_back({"source", "hook", "hash-b"});
    EXPECT_FALSE(acecode::save_hook_trust_store_to_path(
        changed, acecode::path_to_utf8(bad_target), &error));

    loaded = acecode::load_hook_trust_store_from_path(acecode::path_to_utf8(path), &error);
    ASSERT_EQ(loaded.trusted.size(), 1u);
    EXPECT_EQ(loaded.trusted[0].definition_hash, "hash-a");
}
