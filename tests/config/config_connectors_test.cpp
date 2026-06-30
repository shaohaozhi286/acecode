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
