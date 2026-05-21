// Covers ace_browser_bridge config defaults, validation, and persistence.

#include <gtest/gtest.h>

#include "config/config.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

using namespace acecode;

namespace {

bool has_error_containing(const std::vector<std::string>& errors, const std::string& needle) {
    for (const auto& error : errors) {
        if (error.find(needle) != std::string::npos) return true;
    }
    return false;
}

} // namespace

TEST(ConfigAceBrowserBridgeDefaults, StructDefault) {
    AceBrowserBridgeConfig c;
    EXPECT_FALSE(c.enabled);
    EXPECT_TRUE(c.host_path.empty());
    EXPECT_EQ(c.tool_mode, "progressive");
    EXPECT_EQ(c.default_mode, "auto");
    EXPECT_EQ(c.pointer_speed, "normal");
    EXPECT_EQ(c.status_cache_ttl_ms, 2000);
    EXPECT_EQ(c.tool_timeout_ms, 30000);
    EXPECT_FALSE(c.os_pointer_enabled);
    EXPECT_TRUE(c.tab_group_enabled);
    EXPECT_TRUE(c.operation_overlay_enabled);
    EXPECT_EQ(c.operation_overlay_watchdog_ms, 10000);
}

TEST(ConfigAceBrowserBridgeDefaults, NestedInAppConfigIsValid) {
    AppConfig cfg;
    EXPECT_TRUE(validate_config(cfg).empty());
    EXPECT_FALSE(cfg.ace_browser_bridge.enabled);
}

TEST(ConfigAceBrowserBridgeValidation, AcceptsValidEnumValues) {
    for (const std::string& tool_mode : {"progressive", "compact", "full"}) {
        AppConfig cfg;
        cfg.ace_browser_bridge.tool_mode = tool_mode;
        EXPECT_TRUE(validate_config(cfg).empty()) << tool_mode;
    }
    for (const std::string& mode : {"auto", "dom", "cdp", "os"}) {
        AppConfig cfg;
        cfg.ace_browser_bridge.default_mode = mode;
        EXPECT_TRUE(validate_config(cfg).empty()) << mode;
    }
    for (const std::string& speed : {"fast", "normal", "slow", "custom"}) {
        AppConfig cfg;
        cfg.ace_browser_bridge.pointer_speed = speed;
        EXPECT_TRUE(validate_config(cfg).empty()) << speed;
    }
}

TEST(ConfigAceBrowserBridgeValidation, RejectsInvalidEnumValues) {
    {
        AppConfig cfg;
        cfg.ace_browser_bridge.tool_mode = "everything";
        auto errors = validate_config(cfg);
        EXPECT_TRUE(has_error_containing(errors, "tool_mode"));
    }
    {
        AppConfig cfg;
        cfg.ace_browser_bridge.default_mode = "playwright";
        auto errors = validate_config(cfg);
        EXPECT_TRUE(has_error_containing(errors, "default_mode"));
    }
    {
        AppConfig cfg;
        cfg.ace_browser_bridge.pointer_speed = "instant";
        auto errors = validate_config(cfg);
        EXPECT_TRUE(has_error_containing(errors, "pointer_speed"));
    }
}

TEST(ConfigAceBrowserBridgeValidation, RejectsInvalidPointerRanges) {
    {
        AppConfig cfg;
        cfg.ace_browser_bridge.pointer_custom.move_duration_ms_min = 700;
        cfg.ace_browser_bridge.pointer_custom.move_duration_ms_max = 100;
        auto errors = validate_config(cfg);
        EXPECT_TRUE(has_error_containing(errors, "move duration"));
    }
    {
        AppConfig cfg;
        cfg.ace_browser_bridge.pointer_custom.jitter_px = 99.0;
        auto errors = validate_config(cfg);
        EXPECT_TRUE(has_error_containing(errors, "jitter"));
    }
    {
        AppConfig cfg;
        cfg.ace_browser_bridge.operation_overlay_watchdog_ms = 10;
        auto errors = validate_config(cfg);
        EXPECT_TRUE(has_error_containing(errors, "operation_overlay_watchdog_ms"));
    }
}

TEST(ConfigAceBrowserBridgeSave, PersistsNonDefaultValues) {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
        ("acecode-browser-bridge-config-test-" + std::to_string(suffix) + ".json");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    AppConfig cfg;
    cfg.ace_browser_bridge.enabled = true;
    cfg.ace_browser_bridge.tool_mode = "full";
    cfg.ace_browser_bridge.default_mode = "cdp";
    cfg.ace_browser_bridge.pointer_speed = "fast";
    cfg.ace_browser_bridge.pointer_custom.jitter_px = 3.5;
    cfg.ace_browser_bridge.operation_overlay_watchdog_ms = 20000;
    save_config(cfg, path.string());

    std::ifstream ifs(path);
    ASSERT_TRUE(ifs.is_open());
    const auto j = nlohmann::json::parse(ifs);
    ASSERT_TRUE(j.contains("ace_browser_bridge"));
    const auto& abj = j["ace_browser_bridge"];
    EXPECT_EQ(abj["enabled"], true);
    EXPECT_FALSE(abj.contains("host_path"));
    EXPECT_EQ(abj["tool_mode"], "full");
    EXPECT_EQ(abj["default_mode"], "cdp");
    EXPECT_EQ(abj["pointer_speed"], "fast");
    EXPECT_EQ(abj["pointer_custom"]["jitter_px"], 3.5);
    EXPECT_EQ(abj["operation_overlay_watchdog_ms"], 20000);

    std::filesystem::remove(path, ec);
}
