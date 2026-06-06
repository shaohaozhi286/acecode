// 覆盖 Web UI 偏好配置段的默认值、解析兼容和落盘语义。

#include <gtest/gtest.h>

#include "config/config.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

using namespace acecode;

namespace {

void apply_web_ui_section(const nlohmann::json& j, WebUiPreferencesConfig& out) {
    (void)out;
    if (!j.contains("web_ui")) return;
    if (!j["web_ui"].is_object()) return;
}

} // namespace

TEST(ConfigWebUiPreferencesDefaults, StructDefaultHidesAvatar) {
    WebUiPreferencesConfig prefs;
    EXPECT_FALSE(prefs.show_acecode_avatar);
}

TEST(ConfigWebUiPreferencesDefaults, NestedInAppConfigHidesAvatar) {
    AppConfig cfg;
    EXPECT_FALSE(cfg.web_ui.show_acecode_avatar);
}

TEST(ConfigWebUiPreferencesLoader, MissingBlockKeepsDefault) {
    WebUiPreferencesConfig prefs;
    apply_web_ui_section(nlohmann::json::object(), prefs);
    EXPECT_FALSE(prefs.show_acecode_avatar);
}

TEST(ConfigWebUiPreferencesLoader, LegacyExplicitTrueIsIgnored) {
    WebUiPreferencesConfig prefs;
    nlohmann::json j = {{"web_ui", {{"show_acecode_avatar", true}}}};
    apply_web_ui_section(j, prefs);
    EXPECT_FALSE(prefs.show_acecode_avatar);
}

TEST(ConfigWebUiPreferencesLoader, WrongFieldTypeKeepsDefault) {
    WebUiPreferencesConfig prefs;
    nlohmann::json j = {{"web_ui", {{"show_acecode_avatar", "false"}}}};
    apply_web_ui_section(j, prefs);
    EXPECT_FALSE(prefs.show_acecode_avatar);
}

TEST(ConfigWebUiPreferencesSave, DoesNotPersistDisabledDefault) {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
        ("acecode-web-ui-prefs-config-test-" + std::to_string(suffix) + ".json");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    AppConfig cfg;
    cfg.web_ui.show_acecode_avatar = false;
    save_config(cfg, path.string());

    std::ifstream ifs(path);
    ASSERT_TRUE(ifs.is_open());
    const auto j = nlohmann::json::parse(ifs);
    EXPECT_FALSE(j.contains("web_ui"));

    std::filesystem::remove(path, ec);
}
