// 覆盖 src/config/config.{hpp,cpp} 中 desktop.close_to_tray 字段的解析与默认值。
// 设计参见 openspec/changes/enhance-desktop-tray-menu。
//
// 验收点:
//   - DesktopConfig::close_to_tray 默认 true
//   - desktop 段缺失 / desktop.close_to_tray 缺失 → 默认 true
//   - 显式 false → 读入 false
//   - 显式 true → 读入 true(用 false 的 cfg 也能被覆盖回来)
//   - 字段类型错误(应该 bool 给了 string)→ 静默保持默认 true
//
// 与 config_desktop_notifications_test.cpp 风格一致:复刻 load_config 的解析分支,
// 不依赖真实 config.json,避免污染 ~/.acecode/。

#include <gtest/gtest.h>

#include "config/config.hpp"

#include <nlohmann/json.hpp>

using namespace acecode;

namespace {

// 模拟 src/config/config.cpp 中 load_config 对 "desktop.close_to_tray" 的解析逻辑。
// 单字段类型错静默忽略 + 保持默认(与 input_history.enabled 等惯例一致);此处不计 warning。
void apply_close_to_tray(const nlohmann::json& j, DesktopConfig& out) {
    if (!j.contains("desktop")) return;
    if (!j["desktop"].is_object()) return;
    const auto& dj = j["desktop"];
    if (dj.contains("close_to_tray") && dj["close_to_tray"].is_boolean()) {
        out.close_to_tray = dj["close_to_tray"].get<bool>();
    }
}

} // namespace

// 场景:DesktopConfig::close_to_tray 默认值是 true
TEST(ConfigDesktopCloseToTrayDefaults, StructDefault) {
    DesktopConfig d;
    EXPECT_TRUE(d.close_to_tray);
}

// 场景:AppConfig 中 desktop.close_to_tray 默认 true
TEST(ConfigDesktopCloseToTrayDefaults, NestedInAppConfig) {
    AppConfig cfg;
    EXPECT_TRUE(cfg.desktop.close_to_tray);
}

// 场景:config.json 完全没有 desktop 段 → 默认 true
TEST(ConfigDesktopCloseToTrayLoader, MissingDesktopBlockKeepsDefault) {
    DesktopConfig d;
    nlohmann::json j = nlohmann::json::object();
    apply_close_to_tray(j, d);
    EXPECT_TRUE(d.close_to_tray);
}

// 场景:desktop 段存在但缺 close_to_tray 字段 → 默认 true
TEST(ConfigDesktopCloseToTrayLoader, MissingFieldKeepsDefault) {
    DesktopConfig d;
    nlohmann::json j = {{"desktop", nlohmann::json::object()}};
    apply_close_to_tray(j, d);
    EXPECT_TRUE(d.close_to_tray);
}

// 场景:显式 close_to_tray=false 被读入
TEST(ConfigDesktopCloseToTrayLoader, ExplicitFalseRead) {
    DesktopConfig d;
    nlohmann::json j = {{"desktop", {{"close_to_tray", false}}}};
    apply_close_to_tray(j, d);
    EXPECT_FALSE(d.close_to_tray);
}

// 场景:显式 close_to_tray=true(从已被改成 false 的 cfg 还原)
TEST(ConfigDesktopCloseToTrayLoader, ExplicitTrueOverridesFalseDefault) {
    DesktopConfig d;
    d.close_to_tray = false; // 模拟先被改过的状态
    nlohmann::json j = {{"desktop", {{"close_to_tray", true}}}};
    apply_close_to_tray(j, d);
    EXPECT_TRUE(d.close_to_tray);
}

// 场景:字段类型错(给了 string 而非 bool)→ 静默忽略,保持默认 true
TEST(ConfigDesktopCloseToTrayLoader, WrongFieldTypeKeepsDefault) {
    DesktopConfig d;
    nlohmann::json j = {{"desktop", {{"close_to_tray", "yes"}}}};
    apply_close_to_tray(j, d);
    EXPECT_TRUE(d.close_to_tray);
}

// 场景:desktop 段整个非 object → 不解析,保持默认 true
TEST(ConfigDesktopCloseToTrayLoader, DesktopBlockWrongTypeKeepsDefault) {
    DesktopConfig d;
    nlohmann::json j = {{"desktop", "should be object"}};
    apply_close_to_tray(j, d);
    EXPECT_TRUE(d.close_to_tray);
}
