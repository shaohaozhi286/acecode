#include "config/config.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

using namespace acecode;

namespace {

nlohmann::json read_json_file(const std::filesystem::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    return nlohmann::json::parse(ifs);
}

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
