#include "config/config.hpp"
#include "utils/paths.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <stdlib.h>
#endif

using namespace acecode;

namespace {

nlohmann::json read_json_file(const std::filesystem::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    return nlohmann::json::parse(ifs);
}

#ifdef _WIN32
constexpr const char* kHomeEnvName = "USERPROFILE";
#else
constexpr const char* kHomeEnvName = "HOME";
#endif

void set_env_value(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void clear_env_value(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

std::filesystem::path make_temp_home() {
    return std::filesystem::temp_directory_path() /
           ("acecode-connectors-lenient-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

// RAII helper that redirects $HOME/%USERPROFILE% to an isolated temp dir for
// the duration of the test, so acecode::load_config()/save_config() (which
// resolve the config path off the process home dir) read/write a scratch
// config.json instead of the developer's real ~/.acecode/config.json.
// Mirrors the idiom in config_upgrade_test.cpp's LoadConfigReadsUpgradeAndEnvOverride.
class ScopedTempHome {
public:
    ScopedTempHome() : temp_home_(make_temp_home()) {
        acecode::reset_run_mode_for_test();
        if (const char* existing = std::getenv(kHomeEnvName)) {
            previous_home_ = existing;
            had_previous_home_ = true;
        }
        std::filesystem::create_directories(temp_home_ / ".acecode");
        set_env_value(kHomeEnvName, temp_home_.string());
    }

    ~ScopedTempHome() {
        if (had_previous_home_) {
            set_env_value(kHomeEnvName, previous_home_.c_str());
        } else {
            clear_env_value(kHomeEnvName);
        }
        acecode::reset_run_mode_for_test();
        std::error_code ec;
        std::filesystem::remove_all(temp_home_, ec);
    }

    std::filesystem::path config_path() const {
        return temp_home_ / ".acecode" / "config.json";
    }

private:
    std::filesystem::path temp_home_;
    std::string previous_home_;
    bool had_previous_home_ = false;
};

} // namespace

TEST(ConfigConnectors, ParseStrictConnectorArray) {
    auto j = nlohmann::json::parse(R"([
        {
            "id": " alpha-connector ",
            "name": "Alpha Connector",
            "description": "Connect alpha providers",
            "enabled": true
        },
        {
            "id": "beta-connector",
            "name": "Beta Connector",
            "description": "Remote conversation channel",
            "enabled": false
        }
    ])");

    std::vector<ConnectorConfig> connectors;
    std::string error;
    ASSERT_TRUE(parse_connectors_json(j, connectors, &error)) << error;
    ASSERT_EQ(connectors.size(), 2u);
    EXPECT_EQ(connectors[0].id, "alpha-connector");
    EXPECT_EQ(connectors[0].name, "Alpha Connector");
    EXPECT_TRUE(connectors[0].enabled);
    EXPECT_EQ(connectors[1].id, "beta-connector");
    EXPECT_FALSE(connectors[1].enabled);
}

TEST(ConfigConnectors, ParseRejectsDuplicateIds) {
    auto j = nlohmann::json::parse(R"([
        {"id":"same","name":"One","description":"","enabled":true},
        {"id":"same","name":"Two","description":"","enabled":false}
    ])");

    std::vector<ConnectorConfig> connectors;
    std::string error;
    EXPECT_FALSE(parse_connectors_json(j, connectors, &error));
    EXPECT_NE(error.find("unique"), std::string::npos) << error;
}

TEST(ConfigConnectors, SaveOmitsEmptyAndPersistsConfiguredConnectors) {
    auto dir = std::filesystem::temp_directory_path() / "acecode-connectors-config-test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    auto path = dir / "config.json";

    AppConfig cfg;
    save_config(cfg, path.string());
    auto empty_json = read_json_file(path);
    EXPECT_FALSE(empty_json.contains("connectors"));

    cfg.connectors.push_back({
        "alpha-connector",
        "Alpha Connector",
        "Connect alpha providers",
        true,
    });
    cfg.connectors.push_back({
        "beta-connector",
        "Beta Connector",
        "Remote conversation channel",
        false,
    });
    save_config(cfg, path.string());
    auto saved = read_json_file(path);
    ASSERT_TRUE(saved.contains("connectors"));
    ASSERT_TRUE(saved["connectors"].is_array());
    ASSERT_EQ(saved["connectors"].size(), 2u);
    EXPECT_EQ(saved["connectors"][0]["id"], "alpha-connector");
    EXPECT_EQ(saved["connectors"][0]["enabled"], true);
    EXPECT_EQ(saved["connectors"][1]["id"], "beta-connector");
    EXPECT_EQ(saved["connectors"][1]["enabled"], false);

    std::filesystem::remove_all(dir, ec);
}

TEST(ConfigValidation, ConnectorIdsMustBeUnique) {
    AppConfig cfg;
    cfg.connectors.push_back({"same", "One", "", true});
    cfg.connectors.push_back({"same", "Two", "", false});

    auto errs = validate_config(cfg);
    bool found = false;
    for (const auto& e : errs) {
        if (e.find("connectors.id") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(ConfigConnectors, ParseHooksAndScope) {
    auto j = nlohmann::json::parse(R"([
        {
            "id": "gamma",
            "name": "Gamma",
            "description": "with hooks",
            "enabled": true,
            "hooks": {
                "on_enable": {"command": "C:/tools/helper.exe", "args": ["--ensure"], "timeout_ms": 120000},
                "on_auth_error": {"command": "C:/tools/helper.exe"}
            },
            "auth_error_scope": {"base_url_prefix": "https://models.example.com"}
        }
    ])");
    std::vector<ConnectorConfig> connectors;
    std::string error;
    ASSERT_TRUE(parse_connectors_json(j, connectors, &error)) << error;
    ASSERT_EQ(connectors.size(), 1u);
    ASSERT_TRUE(connectors[0].on_enable.has_value());
    EXPECT_EQ(connectors[0].on_enable->command, "C:/tools/helper.exe");
    ASSERT_EQ(connectors[0].on_enable->args.size(), 1u);
    EXPECT_EQ(connectors[0].on_enable->args[0], "--ensure");
    EXPECT_EQ(connectors[0].on_enable->timeout_ms, 120000);
    ASSERT_TRUE(connectors[0].on_auth_error.has_value());
    EXPECT_TRUE(connectors[0].on_auth_error->args.empty());
    EXPECT_EQ(connectors[0].on_auth_error->timeout_ms, 300000);
    EXPECT_EQ(connectors[0].auth_error_base_url_prefix, "https://models.example.com");
}

TEST(ConfigConnectors, HooksRoundTripThroughJson) {
    ConnectorConfig c;
    c.id = "gamma";
    c.name = "Gamma";
    c.description = "with hooks";
    c.enabled = true;
    ConnectorHookConfig hook;
    hook.command = "C:/tools/helper.exe";
    hook.args = {"--ensure"};
    hook.timeout_ms = 120000;
    c.on_enable = hook;
    c.auth_error_base_url_prefix = "https://models.example.com";

    auto j = connectors_to_json({c});
    std::vector<ConnectorConfig> parsed;
    std::string error;
    ASSERT_TRUE(parse_connectors_json(j, parsed, &error)) << error;
    ASSERT_EQ(parsed.size(), 1u);
    ASSERT_TRUE(parsed[0].on_enable.has_value());
    EXPECT_EQ(parsed[0].on_enable->command, "C:/tools/helper.exe");
    EXPECT_EQ(parsed[0].on_enable->timeout_ms, 120000);
    EXPECT_FALSE(parsed[0].on_auth_error.has_value());
    EXPECT_EQ(parsed[0].auth_error_base_url_prefix, "https://models.example.com");
}

TEST(ConfigConnectors, PlainConnectorWithoutHooksStillParsesAndOmitsHooksInJson) {
    auto j = nlohmann::json::parse(R"([
        {"id":"plain","name":"Plain","description":"","enabled":false}
    ])");
    std::vector<ConnectorConfig> connectors;
    std::string error;
    ASSERT_TRUE(parse_connectors_json(j, connectors, &error)) << error;
    EXPECT_FALSE(connectors[0].on_enable.has_value());
    EXPECT_FALSE(connectors[0].on_auth_error.has_value());
    EXPECT_TRUE(connectors[0].auth_error_base_url_prefix.empty());

    auto out = connectors_to_json(connectors);
    EXPECT_FALSE(out[0].contains("hooks"));
    EXPECT_FALSE(out[0].contains("auth_error_scope"));
}

TEST(ConfigConnectors, RejectsHookWithoutCommand) {
    auto j = nlohmann::json::parse(R"([
        {"id":"bad","name":"Bad","description":"","enabled":true,
         "hooks":{"on_enable":{"args":["x"]}}}
    ])");
    std::vector<ConnectorConfig> connectors;
    std::string error;
    EXPECT_FALSE(parse_connectors_json(j, connectors, &error));
    EXPECT_NE(error.find("on_enable"), std::string::npos);
}

TEST(ConfigConnectors, NewlyEnabledConnectorsDetectsOffToOnWithHook) {
    ConnectorHookConfig hook;
    hook.command = "helper.exe";

    ConnectorConfig was_off;
    was_off.id = "a";
    was_off.name = "A";
    was_off.enabled = false;
    was_off.on_enable = hook;

    ConnectorConfig stays_on = was_off;
    stays_on.id = "b";
    stays_on.name = "B";
    stays_on.enabled = true;

    ConnectorConfig no_hook;
    no_hook.id = "c";
    no_hook.name = "C";
    no_hook.enabled = false;

    std::vector<ConnectorConfig> before = {was_off, stays_on, no_hook};

    auto now_on = was_off;
    now_on.enabled = true;
    auto still_on = stays_on;
    auto hook_off_to_on_but_no_hook = no_hook;
    hook_off_to_on_but_no_hook.enabled = true;
    std::vector<ConnectorConfig> after = {now_on, still_on, hook_off_to_on_but_no_hook};

    auto newly = newly_enabled_connectors(before, after);
    ASSERT_EQ(newly.size(), 1u);
    EXPECT_EQ(newly[0].id, "a");
}

TEST(ConfigConnectors, NewlyEnabledTreatsUnknownIdAsNewlyEnabled) {
    ConnectorHookConfig hook;
    hook.command = "helper.exe";
    ConnectorConfig fresh;
    fresh.id = "new";
    fresh.name = "New";
    fresh.enabled = true;
    fresh.on_enable = hook;

    auto newly = newly_enabled_connectors({}, {fresh});
    ASSERT_EQ(newly.size(), 1u);
    EXPECT_EQ(newly[0].id, "new");
}

// Regression coverage for the lenient loader used by load_config() (the path
// GET /api/config/connectors and ConnectorAuthRecovery::recover() both go
// through). load_connectors_lenient() is file-internal to config.cpp (not
// declared in config.hpp), so it's exercised indirectly through load_config()
// via a redirected $HOME/%USERPROFILE%, following the ScopedTempHome idiom
// used by config_first_init_test.cpp / config_upgrade_test.cpp.
//
// Bug: load_connectors_lenient() only ever parsed id/name/description/enabled
// and silently dropped hooks + auth_error_scope on every disk read. That
// meant (1) PUT /api/config/connectors's GET-mutate-PUT-save round trip wiped
// hooks from disk on a mere enable/disable toggle, and (2)
// ConnectorAuthRecovery::recover() read connectors via load_config() and so
// on_auth_error was always nullopt -- the chat-400 auto-recovery feature
// never matched anything.
TEST(ConfigConnectorsLenientLoad, LoadConfigParsesHooksAndAuthErrorScope) {
    ScopedTempHome home;
    {
        std::ofstream ofs(home.config_path());
        ofs << R"({
    "provider": "",
    "saved_models": [],
    "connectors": [
        {
            "id": "gamma",
            "name": "Gamma",
            "description": "with hooks",
            "enabled": true,
            "hooks": {
                "on_enable": {"command": "C:/tools/helper.exe", "args": ["--ensure"], "timeout_ms": 120000},
                "on_auth_error": {"command": "C:/tools/helper.exe"}
            },
            "auth_error_scope": {"base_url_prefix": "https://models.example.com"}
        }
    ]
})";
    }

    auto cfg = acecode::load_config();

    ASSERT_EQ(cfg.connectors.size(), 1u);
    const auto& connector = cfg.connectors[0];
    EXPECT_EQ(connector.id, "gamma");

    ASSERT_TRUE(connector.on_enable.has_value())
        << "load_connectors_lenient() dropped hooks.on_enable";
    EXPECT_EQ(connector.on_enable->command, "C:/tools/helper.exe");
    ASSERT_EQ(connector.on_enable->args.size(), 1u);
    EXPECT_EQ(connector.on_enable->args[0], "--ensure");
    EXPECT_EQ(connector.on_enable->timeout_ms, 120000);

    ASSERT_TRUE(connector.on_auth_error.has_value())
        << "load_connectors_lenient() dropped hooks.on_auth_error "
           "(ConnectorAuthRecovery::recover() can never match)";
    EXPECT_EQ(connector.on_auth_error->command, "C:/tools/helper.exe");
    EXPECT_TRUE(connector.on_auth_error->args.empty());
    EXPECT_EQ(connector.on_auth_error->timeout_ms, 300000);

    EXPECT_EQ(connector.auth_error_base_url_prefix, "https://models.example.com")
        << "load_connectors_lenient() dropped auth_error_scope.base_url_prefix";
}

TEST(ConfigConnectorsLenientLoad, MalformedHookIsDroppedButConnectorSurvives) {
    ScopedTempHome home;
    {
        std::ofstream ofs(home.config_path());
        ofs << R"({
    "provider": "",
    "saved_models": [],
    "connectors": [
        {
            "id": "bad-hook",
            "name": "Bad Hook",
            "description": "hook missing command",
            "enabled": true,
            "hooks": {
                "on_enable": {"args": ["--ensure"]}
            },
            "auth_error_scope": {"base_url_prefix": " https://models.example.com "}
        }
    ]
})";
    }

    auto cfg = acecode::load_config();

    ASSERT_EQ(cfg.connectors.size(), 1u)
        << "a malformed hook should drop only the hook, not the whole connector";
    const auto& connector = cfg.connectors[0];
    EXPECT_EQ(connector.id, "bad-hook");
    EXPECT_EQ(connector.name, "Bad Hook");
    EXPECT_TRUE(connector.enabled);
    EXPECT_FALSE(connector.on_enable.has_value())
        << "malformed hooks.on_enable (missing command) should be ignored, not accepted";
    // auth_error_scope in the same entry is well-formed and should still parse
    // (trimmed), independent of the sibling malformed hook.
    EXPECT_EQ(connector.auth_error_base_url_prefix, "https://models.example.com");
}

TEST(ConfigConnectorsLenientLoad, MalformedAuthErrorScopeIsIgnoredButConnectorSurvives) {
    ScopedTempHome home;
    {
        std::ofstream ofs(home.config_path());
        ofs << R"({
    "provider": "",
    "saved_models": [],
    "connectors": [
        {
            "id": "bad-scope",
            "name": "Bad Scope",
            "description": "auth_error_scope.base_url_prefix not a string",
            "enabled": true,
            "auth_error_scope": {"base_url_prefix": 12345}
        }
    ]
})";
    }

    auto cfg = acecode::load_config();

    ASSERT_EQ(cfg.connectors.size(), 1u);
    const auto& connector = cfg.connectors[0];
    EXPECT_EQ(connector.id, "bad-scope");
    EXPECT_TRUE(connector.auth_error_base_url_prefix.empty());
}

// The disk-wipe regression: GET /api/config/connectors reads via load_config()
// (lenient path), the web UI mutates the in-memory list (e.g. toggling
// `enabled`), and PUT saves the whole list back via save_config(). If the
// lenient loader dropped hooks on the way in, this round trip silently wipes
// them from disk even though the caller never touched them.
TEST(ConfigConnectors, ParseOnStartupHook) {
    auto j = nlohmann::json::parse(R"([
        {"id":"delta","name":"Delta","description":"","enabled":true,
         "hooks":{"on_startup":{"command":"C:/tools/helper.exe","args":["--check"],"timeout_ms":60000}}}
    ])");
    std::vector<ConnectorConfig> connectors;
    std::string error;
    ASSERT_TRUE(parse_connectors_json(j, connectors, &error)) << error;
    ASSERT_TRUE(connectors[0].on_startup.has_value());
    EXPECT_EQ(connectors[0].on_startup->command, "C:/tools/helper.exe");
    EXPECT_EQ(connectors[0].on_startup->timeout_ms, 60000);
    EXPECT_FALSE(connectors[0].on_enable.has_value());
}

TEST(ConfigConnectors, OnStartupSurvivesStrictRoundTrip) {
    ConnectorConfig c;
    c.id = "delta";
    c.name = "Delta";
    c.description = "";
    c.enabled = true;
    ConnectorHookConfig hook;
    hook.command = "C:/tools/helper.exe";
    c.on_startup = hook;

    auto j = connectors_to_json({c});
    std::vector<ConnectorConfig> parsed;
    std::string error;
    ASSERT_TRUE(parse_connectors_json(j, parsed, &error)) << error;
    ASSERT_TRUE(parsed[0].on_startup.has_value());
    EXPECT_EQ(parsed[0].on_startup->command, "C:/tools/helper.exe");
    EXPECT_EQ(parsed[0].on_startup->timeout_ms, 300000);
}

TEST(ConfigConnectorsLenientLoad, RoundTripThroughLenientLoadAndSavePreservesHooks) {
    ScopedTempHome home;
    {
        std::ofstream ofs(home.config_path());
        ofs << R"({
    "provider": "",
    "saved_models": [],
    "connectors": [
        {
            "id": "gamma",
            "name": "Gamma",
            "description": "with hooks",
            "enabled": false,
            "hooks": {
                "on_enable": {"command": "C:/tools/helper.exe", "args": ["--ensure"], "timeout_ms": 120000},
                "on_auth_error": {"command": "C:/tools/helper.exe", "timeout_ms": 60000},
                "on_startup": {"command": "C:/tools/helper.exe"}
            },
            "auth_error_scope": {"base_url_prefix": "https://models.example.com"}
        }
    ]
})";
    }

    // 1. Load via the lenient path (as GET /api/config/connectors does).
    auto cfg = acecode::load_config();
    ASSERT_EQ(cfg.connectors.size(), 1u);

    // 2. Mutate something unrelated to hooks, mirroring a PUT that only
    //    flips `enabled` (the web UI's toggle action).
    cfg.connectors[0].enabled = true;

    // 3. Save back to disk (as PUT /api/config/connectors does).
    acecode::save_config(cfg, home.config_path().string());

    // 4. Reload via the lenient path again and confirm hooks/scope survived
    //    the round trip instead of being silently wiped.
    auto reloaded = acecode::load_config();
    ASSERT_EQ(reloaded.connectors.size(), 1u);
    const auto& reloaded_connector = reloaded.connectors[0];
    EXPECT_TRUE(reloaded_connector.enabled);

    ASSERT_TRUE(reloaded_connector.on_enable.has_value())
        << "hooks.on_enable was wiped by the lenient load -> save round trip";
    EXPECT_EQ(reloaded_connector.on_enable->command, "C:/tools/helper.exe");
    ASSERT_EQ(reloaded_connector.on_enable->args.size(), 1u);
    EXPECT_EQ(reloaded_connector.on_enable->args[0], "--ensure");
    EXPECT_EQ(reloaded_connector.on_enable->timeout_ms, 120000);

    ASSERT_TRUE(reloaded_connector.on_auth_error.has_value())
        << "hooks.on_auth_error was wiped by the lenient load -> save round trip";
    EXPECT_EQ(reloaded_connector.on_auth_error->command, "C:/tools/helper.exe");
    EXPECT_EQ(reloaded_connector.on_auth_error->timeout_ms, 60000);

    ASSERT_TRUE(reloaded_connector.on_startup.has_value());
    EXPECT_EQ(reloaded_connector.on_startup->command, "C:/tools/helper.exe");

    EXPECT_EQ(reloaded_connector.auth_error_base_url_prefix, "https://models.example.com")
        << "auth_error_scope.base_url_prefix was wiped by the lenient load -> save round trip";
}

TEST(ConfigConnectors, StartupHookConnectorsFiltersEnabledWithHook) {
    ConnectorHookConfig hook;
    hook.command = "helper.exe";

    ConnectorConfig enabled_with_hook;
    enabled_with_hook.id = "a";
    enabled_with_hook.name = "A";
    enabled_with_hook.enabled = true;
    enabled_with_hook.on_startup = hook;

    ConnectorConfig disabled_with_hook = enabled_with_hook;
    disabled_with_hook.id = "b";
    disabled_with_hook.enabled = false;

    ConnectorConfig enabled_no_hook;
    enabled_no_hook.id = "c";
    enabled_no_hook.name = "C";
    enabled_no_hook.enabled = true;

    auto out = startup_hook_connectors({enabled_with_hook, disabled_with_hook, enabled_no_hook});
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].id, "a");
}
