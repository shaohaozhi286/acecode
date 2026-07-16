// 覆盖 src/config/config.{hpp,cpp} 中 desktop.notifications 段的解析与默认值。
// 设计参见 openspec/changes/add-windows-wintoast-completion-notifications。
//
// 验收点:
//   - DesktopNotificationsConfig 四个字段默认全 true
//   - desktop / desktop.notifications 段完全缺失 → 默认值,无 warning
//   - 部分字段缺失 → 仅缺的字段走默认,提供的字段被读入
//   - 字段类型错误(应该 bool 给了 string)→ 该字段保留默认 + 整段 warning
//   - desktop 整段非对象 / desktop.notifications 非对象 → 默认值 + warning
//
// 与 config_tui_test 风格一致:复刻 load_config 的解析分支,不依赖真实 config.json,
// 避免污染 ~/.acecode/。

#include <gtest/gtest.h>

#include "config/config.hpp"

#include <nlohmann/json.hpp>

using namespace acecode;

namespace {

// 模拟 src/config/config.cpp 中 load_config 对 "desktop" 段的解析 + 规范化逻辑。
// 返回 warn 计数:desktop / desktop.notifications 类型错各 +1,字段类型错不计 warning
// (单个 bool 字段静默忽略,与现有 input_history.enabled 等惯例一致)。
int apply_desktop_section(const nlohmann::json& j, DesktopNotificationsConfig& out) {
    int warnings = 0;
    if (!j.contains("desktop")) return 0;
    if (!j["desktop"].is_object()) {
        ++warnings;
        return warnings;
    }
    const auto& dj = j["desktop"];
    if (!dj.contains("notifications")) return warnings;
    if (!dj["notifications"].is_object()) {
        ++warnings;
        return warnings;
    }
    const auto& nj = dj["notifications"];
    if (nj.contains("enabled") && nj["enabled"].is_boolean()) {
        out.enabled = nj["enabled"].get<bool>();
    }
    if (nj.contains("on_question") && nj["on_question"].is_boolean()) {
        out.on_question = nj["on_question"].get<bool>();
    }
    if (nj.contains("on_completion") && nj["on_completion"].is_boolean()) {
        out.on_completion = nj["on_completion"].get<bool>();
    }
    if (nj.contains("suppress_when_focused") && nj["suppress_when_focused"].is_boolean()) {
        out.suppress_when_focused = nj["suppress_when_focused"].get<bool>();
    }
    return warnings;
}

} // namespace

// 场景:DesktopNotificationsConfig 默认四个 bool 都是 true
TEST(ConfigDesktopNotificationsDefaults, StructDefault) {
    DesktopNotificationsConfig n;
    EXPECT_TRUE(n.enabled);
    EXPECT_TRUE(n.on_question);
    EXPECT_TRUE(n.on_completion);
    EXPECT_TRUE(n.suppress_when_focused);
}

// 场景:AppConfig 中默认包含 desktop.notifications 且默认全 true
TEST(ConfigDesktopNotificationsDefaults, NestedInAppConfig) {
    AppConfig cfg;
    EXPECT_TRUE(cfg.desktop.notifications.enabled);
    EXPECT_TRUE(cfg.desktop.notifications.on_question);
    EXPECT_TRUE(cfg.desktop.notifications.on_completion);
    EXPECT_TRUE(cfg.desktop.notifications.suppress_when_focused);
}

// 场景:config.json 完全没有 desktop 段 → 默认值,无警告
TEST(ConfigDesktopNotificationsLoader, MissingBlockKeepsDefault) {
    DesktopNotificationsConfig n;
    nlohmann::json j = nlohmann::json::object();
    EXPECT_EQ(apply_desktop_section(j, n), 0);
    EXPECT_TRUE(n.enabled);
    EXPECT_TRUE(n.on_question);
    EXPECT_TRUE(n.on_completion);
    EXPECT_TRUE(n.suppress_when_focused);
}

// 场景:desktop 段存在但缺 notifications 子段 → 默认值,无警告
TEST(ConfigDesktopNotificationsLoader, MissingNotificationsKeepsDefault) {
    DesktopNotificationsConfig n;
    nlohmann::json j = {{"desktop", nlohmann::json::object()}};
    EXPECT_EQ(apply_desktop_section(j, n), 0);
    EXPECT_TRUE(n.enabled);
    EXPECT_TRUE(n.suppress_when_focused);
}

// 场景:desktop.notifications 是空对象 → 默认值,无警告
TEST(ConfigDesktopNotificationsLoader, EmptyNotificationsKeepsDefault) {
    DesktopNotificationsConfig n;
    nlohmann::json j = {{"desktop", {{"notifications", nlohmann::json::object()}}}};
    EXPECT_EQ(apply_desktop_section(j, n), 0);
    EXPECT_TRUE(n.enabled);
}

// 场景:四个字段全部显式给 false → 全部读入
TEST(ConfigDesktopNotificationsLoader, AllFieldsExplicitlyFalse) {
    DesktopNotificationsConfig n;
    nlohmann::json j = {
        {"desktop", {{"notifications", {
            {"enabled", false},
            {"on_question", false},
            {"on_completion", false},
            {"suppress_when_focused", false},
        }}}}
    };
    EXPECT_EQ(apply_desktop_section(j, n), 0);
    EXPECT_FALSE(n.enabled);
    EXPECT_FALSE(n.on_question);
    EXPECT_FALSE(n.on_completion);
    EXPECT_FALSE(n.suppress_when_focused);
}

// 场景:仅显式给 enabled=false,其它三个走默认 true
TEST(ConfigDesktopNotificationsLoader, PartialFieldsRespectsDefaultsForRest) {
    DesktopNotificationsConfig n;
    nlohmann::json j = {
        {"desktop", {{"notifications", {{"enabled", false}}}}}
    };
    EXPECT_EQ(apply_desktop_section(j, n), 0);
    EXPECT_FALSE(n.enabled);
    EXPECT_TRUE(n.on_question);
    EXPECT_TRUE(n.on_completion);
    EXPECT_TRUE(n.suppress_when_focused);
}

// 场景:字段类型错误(给了 string 而非 bool)→ 该字段静默保持默认
TEST(ConfigDesktopNotificationsLoader, WrongFieldTypeKeepsDefaultSilently) {
    DesktopNotificationsConfig n;
    nlohmann::json j = {
        {"desktop", {{"notifications", {{"enabled", "yes"}, {"on_question", false}}}}}
    };
    // enabled 字段类型错被忽略不计 warn(单字段错与 input_history 等段的 ignore-and-default 一致),
    // on_question 仍按 bool 读入。
    EXPECT_EQ(apply_desktop_section(j, n), 0);
    EXPECT_TRUE(n.enabled);     // 类型错被忽略,保持默认 true
    EXPECT_FALSE(n.on_question); // 合法 bool 被读入
}

// 场景:desktop 段类型错(应该是 object 给了 string)→ 默认值 + 1 条 warning
TEST(ConfigDesktopNotificationsLoader, DesktopBlockWrongTypeWarns) {
    DesktopNotificationsConfig n;
    nlohmann::json j = {{"desktop", "should be object"}};
    EXPECT_EQ(apply_desktop_section(j, n), 1);
    EXPECT_TRUE(n.enabled); // 默认值不被破坏
}

// 场景:desktop.notifications 子段类型错 → 默认值 + 1 条 warning
TEST(ConfigDesktopNotificationsLoader, NotificationsBlockWrongTypeWarns) {
    DesktopNotificationsConfig n;
    nlohmann::json j = {{"desktop", {{"notifications", "should be object"}}}};
    EXPECT_EQ(apply_desktop_section(j, n), 1);
    EXPECT_TRUE(n.enabled);
}
