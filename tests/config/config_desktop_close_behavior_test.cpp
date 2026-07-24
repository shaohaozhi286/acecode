#include <gtest/gtest.h>

#include "config/config.hpp"
#include "config/desktop_close_behavior.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace {

fs::path temp_config_path() {
    return fs::temp_directory_path() /
        ("acecode-desktop-close-behavior-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) +
         ".json");
}

nlohmann::json read_json(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return nlohmann::json::parse(input);
}

} // namespace

TEST(ConfigDesktopCloseBehavior, FreshConfigurationAsks) {
    acecode::DesktopConfig desktop;
    acecode::AppConfig app;
    EXPECT_EQ(desktop.close_behavior, acecode::DesktopCloseBehavior::Ask);
    EXPECT_EQ(app.desktop.close_behavior, acecode::DesktopCloseBehavior::Ask);
}

TEST(ConfigDesktopCloseBehavior, ParsesEveryPersistedValue) {
    EXPECT_EQ(acecode::parse_desktop_close_behavior("ask"),
              acecode::DesktopCloseBehavior::Ask);
    EXPECT_EQ(acecode::parse_desktop_close_behavior("minimize_to_tray"),
              acecode::DesktopCloseBehavior::MinimizeToTray);
    EXPECT_EQ(acecode::parse_desktop_close_behavior("exit"),
              acecode::DesktopCloseBehavior::Exit);
    EXPECT_FALSE(acecode::parse_desktop_close_behavior("invalid"));
}

TEST(ConfigDesktopCloseBehavior, NewFieldWinsAndLegacyFieldMigrates) {
    EXPECT_EQ(
        acecode::resolve_desktop_close_behavior(std::nullopt, true),
        acecode::DesktopCloseBehavior::MinimizeToTray);
    EXPECT_EQ(
        acecode::resolve_desktop_close_behavior(std::nullopt, false),
        acecode::DesktopCloseBehavior::Exit);
    EXPECT_EQ(
        acecode::resolve_desktop_close_behavior(std::string_view("ask"), false),
        acecode::DesktopCloseBehavior::Ask);
    EXPECT_EQ(
        acecode::resolve_desktop_close_behavior(std::string_view("invalid"), false),
        acecode::DesktopCloseBehavior::Exit);
}

TEST(ConfigDesktopCloseBehavior, SaveWritesRememberedValueAndOmitsAskDefault) {
    const fs::path remembered_path = temp_config_path();
    acecode::AppConfig remembered;
    remembered.desktop.close_behavior = acecode::DesktopCloseBehavior::Exit;
    remembered.desktop.close_to_tray = false;
    acecode::save_config(remembered, remembered_path.string());
    const auto remembered_json = read_json(remembered_path);
    ASSERT_TRUE(remembered_json.contains("desktop"));
    EXPECT_EQ(remembered_json["desktop"].value("close_behavior", ""), "exit");
    EXPECT_FALSE(remembered_json["desktop"].value("close_to_tray", true));

    const fs::path default_path = temp_config_path();
    acecode::AppConfig defaults;
    acecode::save_config(defaults, default_path.string());
    const auto default_json = read_json(default_path);
    EXPECT_FALSE(default_json.contains("desktop") &&
                 default_json["desktop"].contains("close_behavior"));

    std::error_code ec;
    fs::remove(remembered_path, ec);
    fs::remove(default_path, ec);
}
