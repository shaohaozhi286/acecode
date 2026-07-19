#include <gtest/gtest.h>

#include "config/config.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace {

fs::path temp_config_path() {
    return fs::temp_directory_path() /
        ("acecode-desktop-background-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) +
         ".json");
}

void apply_background_field(const nlohmann::json& root,
                            acecode::DesktopConfig& config) {
    if (!root.contains("desktop") || !root["desktop"].is_object()) return;
    const auto& desktop = root["desktop"];
    if (desktop.contains("continue_background_process") &&
        desktop["continue_background_process"].is_boolean()) {
        config.continue_background_process =
            desktop["continue_background_process"].get<bool>();
    }
}

nlohmann::json read_json(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return nlohmann::json::parse(input);
}

} // namespace

TEST(ConfigDesktopBackgroundProcess, DefaultsToDisabled) {
    acecode::DesktopConfig desktop;
    acecode::AppConfig app;
    EXPECT_FALSE(desktop.continue_background_process);
    EXPECT_FALSE(app.desktop.continue_background_process);
}

TEST(ConfigDesktopBackgroundProcess, MissingOrWrongTypeKeepsDefault) {
    acecode::DesktopConfig missing;
    apply_background_field(nlohmann::json::object(), missing);
    EXPECT_FALSE(missing.continue_background_process);

    acecode::DesktopConfig wrong_type;
    apply_background_field(
        {{"desktop", {{"continue_background_process", "yes"}}}},
        wrong_type);
    EXPECT_FALSE(wrong_type.continue_background_process);
}

TEST(ConfigDesktopBackgroundProcess, ExplicitBooleanLoads) {
    acecode::DesktopConfig desktop;
    apply_background_field(
        {{"desktop", {{"continue_background_process", true}}}},
        desktop);
    EXPECT_TRUE(desktop.continue_background_process);
}

TEST(ConfigDesktopBackgroundProcess, SaveWritesEnabledAndOmitsDefault) {
    const fs::path enabled_path = temp_config_path();
    acecode::AppConfig enabled;
    enabled.desktop.continue_background_process = true;
    acecode::save_config(enabled, enabled_path.string());
    const auto enabled_json = read_json(enabled_path);
    ASSERT_TRUE(enabled_json.contains("desktop"));
    EXPECT_TRUE(
        enabled_json["desktop"].value("continue_background_process", false));

    const fs::path default_path = temp_config_path();
    acecode::AppConfig defaults;
    acecode::save_config(defaults, default_path.string());
    const auto default_json = read_json(default_path);
    EXPECT_FALSE(default_json.contains("desktop") &&
                 default_json["desktop"].contains(
                     "continue_background_process"));

    std::error_code ec;
    fs::remove(enabled_path, ec);
    fs::remove(default_path, ec);
}
