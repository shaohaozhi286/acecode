// 覆盖 src/config/config.{hpp,cpp} 中 tui 段的解析与默认值,以
// add-legacy-terminal-fallback 引入的 schema 为基线:
//   - TuiConfig::alt_screen_mode 默认 "auto"
//   - tui 段缺失 → 默认值,不报错
//   - 合法值("auto" / "always" / "never")原样接受
//   - 非法字符串 → 规范化到 "auto" + 一条 warn
//   - tui 段类型错误(非对象)→ 默认值 + 一条 warn
//
// 与既有 config 测试一致:复刻 load_config 中相关分支而不依赖真实 config.json,
// 避免污染用户 ~/.acecode/ 也方便独立验证规范化逻辑。

#include <gtest/gtest.h>

#include "config/config.hpp"

#include <nlohmann/json.hpp>

using namespace acecode;

namespace {

// 模拟 config.cpp 中 load_config 对 "tui" 段的解析 + 规范化逻辑。
// 返回 warn 计数;非对象类型 / 非法字符串各算一条。
int apply_tui_section(const nlohmann::json& j_with_tui, TuiConfig& out) {
    int warnings = 0;
    if (!j_with_tui.contains("tui")) return 0;
    if (!j_with_tui["tui"].is_object()) {
        ++warnings;
        return warnings;
    }
    const auto& tj = j_with_tui["tui"];
    if (tj.contains("alt_screen_mode") && tj["alt_screen_mode"].is_string()) {
        std::string m = tj["alt_screen_mode"].get<std::string>();
        if (m == "auto" || m == "always" || m == "never") {
            out.alt_screen_mode = std::move(m);
        } else {
            ++warnings;
            out.alt_screen_mode = "auto";
        }
    }
    return warnings;
}

} // namespace

// 场景:TuiConfig 默认 alt_screen_mode 为 "auto"
TEST(ConfigTuiDefaults, StructDefault) {
    TuiConfig t;
    EXPECT_EQ(t.alt_screen_mode, "auto");
}

// 场景:AppConfig 中默认包含 tui 字段且默认 "auto"
TEST(ConfigTuiDefaults, NestedInAppConfig) {
    AppConfig cfg;
    EXPECT_EQ(cfg.tui.alt_screen_mode, "auto");
}

// 场景:config.json 完全没有 tui 段 → 默认值,无警告
TEST(ConfigTuiLoader, MissingBlockKeepsDefault) {
    TuiConfig t;
    nlohmann::json j = nlohmann::json::object();
    EXPECT_EQ(apply_tui_section(j, t), 0);
    EXPECT_EQ(t.alt_screen_mode, "auto");
}

// 场景:tui 段是空对象 → 默认值不变
TEST(ConfigTuiLoader, EmptyBlockKeepsDefault) {
    TuiConfig t;
    nlohmann::json j = {{"tui", nlohmann::json::object()}};
    EXPECT_EQ(apply_tui_section(j, t), 0);
    EXPECT_EQ(t.alt_screen_mode, "auto");
}

// 场景:三种合法字符串都被原样接受
TEST(ConfigTuiLoader, AcceptsAllValidValues) {
    for (const std::string& v : {"auto", "always", "never"}) {
        TuiConfig t;
        nlohmann::json j = {{"tui", {{"alt_screen_mode", v}}}};
        EXPECT_EQ(apply_tui_section(j, t), 0) << "value=" << v;
        EXPECT_EQ(t.alt_screen_mode, v);
    }
}

// 场景:非法字符串值 → 规范化到 "auto",触发一条 warn
TEST(ConfigTuiLoader, InvalidStringFallsBackToAuto) {
    TuiConfig t;
    nlohmann::json j = {{"tui", {{"alt_screen_mode", "fullscreen"}}}};
    EXPECT_EQ(apply_tui_section(j, t), 1);
    EXPECT_EQ(t.alt_screen_mode, "auto");
}

// 场景:大小写敏感(比如 "Always" 被视为非法)→ "auto" + warn
TEST(ConfigTuiLoader, CaseSensitiveValueRejected) {
    TuiConfig t;
    nlohmann::json j = {{"tui", {{"alt_screen_mode", "Always"}}}};
    EXPECT_EQ(apply_tui_section(j, t), 1);
    EXPECT_EQ(t.alt_screen_mode, "auto");
}

// 场景:tui 不是对象(数组 / 字符串 / 数字)→ 触发 warn,不应用任何字段
TEST(ConfigTuiLoader, NonObjectIsRejected) {
    {
        TuiConfig t;
        nlohmann::json j = {{"tui", nlohmann::json::array({"a", "b"})}};
        EXPECT_EQ(apply_tui_section(j, t), 1);
        EXPECT_EQ(t.alt_screen_mode, "auto");
    }
    {
        TuiConfig t;
        nlohmann::json j = {{"tui", "always"}};
        EXPECT_EQ(apply_tui_section(j, t), 1);
        EXPECT_EQ(t.alt_screen_mode, "auto");
    }
    {
        TuiConfig t;
        nlohmann::json j = {{"tui", 42}};
        EXPECT_EQ(apply_tui_section(j, t), 1);
        EXPECT_EQ(t.alt_screen_mode, "auto");
    }
}

// 场景:alt_screen_mode 字段类型错误(非字符串)→ 不修改,无 warn
// 注:跟 load_config 的现有约定一致,字段类型不符即静默跳过。
TEST(ConfigTuiLoader, NonStringFieldIgnored) {
    TuiConfig t;
    nlohmann::json j = {{"tui", {{"alt_screen_mode", 42}}}};
    EXPECT_EQ(apply_tui_section(j, t), 0);
    EXPECT_EQ(t.alt_screen_mode, "auto");
}
