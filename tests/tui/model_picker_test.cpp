// tests/tui/model_picker_test.cpp
//
// 覆盖 src/tui/model_picker.cpp 的 build_model_picker_options 纯函数。
// 任一回归(顺序错、(legacy) 重复、is_current 标记错位)都会让 picker
// UI 反映错误的当前模型 / 漏选项 / 漏行,所以这几条 case 都是 picker 正
// 确性的最小护栏。
//
// 触发场景 / 期望(每个 TEST 子句):
//   - 含 (legacy) 名字的 saved_models 不重复追加
//   - current_name 命中 saved_models 某条 → is_current 标记
//   - current_name = "(legacy)" 但 saved_models 不含 → 末尾合成行 is_current
//   - current_name 不命中任何条目 → 全部 is_current=false
//   - 空 saved_models → 只返回单条 (legacy) 兜底

#include "tui/model_picker.hpp"

#include "config/config.hpp"
#include "config/saved_models.hpp"

#include <gtest/gtest.h>

using acecode::AppConfig;
using acecode::build_model_picker_options;
using acecode::ModelPickerOption;
using acecode::ModelProfile;

namespace {

// 构造一份典型的 cfg:provider=copilot/gpt-4o(让 synth_legacy_entry 拿
// 到稳定的回退值),saved_models 里两条:copilot-fast / local-lm。
AppConfig make_cfg_with_two() {
    AppConfig cfg;
    cfg.provider = "copilot";
    cfg.copilot.model = "gpt-4o";
    ModelProfile a;
    a.name = "copilot-fast";
    a.provider = "copilot";
    a.model = "gpt-4o";
    cfg.saved_models.push_back(a);
    ModelProfile b;
    b.name = "local-lm";
    b.provider = "openai";
    b.model = "llama-3";
    b.base_url = "http://localhost:1234/v1";
    b.api_key = "x";
    cfg.saved_models.push_back(b);
    return cfg;
}

} // namespace

// 场景:current_name 命中 saved_models 里某条 → 该条 is_current=true,
// (legacy) 行追加在末尾。回归触发条件:用户配 saved_models + 默认指向
// 第一条;picker 必须把第一条标 *,legacy 当兜底列出。
TEST(ModelPicker, BuildOptionsMarksCurrent) {
    auto cfg = make_cfg_with_two();
    auto opts = build_model_picker_options(cfg, "copilot-fast");
    ASSERT_EQ(opts.size(), 3u);
    EXPECT_EQ(opts[0].name, "copilot-fast");
    EXPECT_TRUE(opts[0].is_current);
    EXPECT_EQ(opts[1].name, "local-lm");
    EXPECT_FALSE(opts[1].is_current);
    EXPECT_EQ(opts[2].name, "(legacy)");
    EXPECT_FALSE(opts[2].is_current);
}

// 场景:current_name 是 "(legacy)" → 末尾的合成 (legacy) 行 is_current=true。
// 触发:用户没配 saved_models,或 swap 走到了 legacy 兜底,picker 必须把
// "*" 落在末尾那条上。
TEST(ModelPicker, LegacyMarkedWhenCurrent) {
    auto cfg = make_cfg_with_two();
    auto opts = build_model_picker_options(cfg, "(legacy)");
    ASSERT_EQ(opts.size(), 3u);
    EXPECT_FALSE(opts[0].is_current);
    EXPECT_FALSE(opts[1].is_current);
    EXPECT_EQ(opts[2].name, "(legacy)");
    EXPECT_TRUE(opts[2].is_current);
}

// 场景:saved_models 里已经有 "(legacy)" → 不重复追加。
// 触发条件极少见(用户手编 cfg.json 把 legacy 当 saved 写,绕过
// validate_saved_models 的保留前缀检查,例如直接 patch 内存里的 cfg
// 跑测试),但要防御列表出现两条 (legacy)。
TEST(ModelPicker, NoDuplicateLegacyWhenSavedHasIt) {
    AppConfig cfg;
    cfg.provider = "copilot";
    cfg.copilot.model = "gpt-4o";
    ModelProfile legacy;
    legacy.name = "(legacy)";
    legacy.provider = "copilot";
    legacy.model = "gpt-4o";
    cfg.saved_models.push_back(legacy);
    auto opts = build_model_picker_options(cfg, "(legacy)");
    ASSERT_EQ(opts.size(), 1u);
    EXPECT_EQ(opts[0].name, "(legacy)");
    EXPECT_TRUE(opts[0].is_current);
}

// 场景:current_name 不在任何条目里 → 全部 is_current=false。
// 触发:resume 一条 ad-hoc "(session:abc)" 然后打开 picker —— picker 不应
// 把 highlight 落到任何 saved_models 行,UI 上没有 *。
TEST(ModelPicker, NoCurrentWhenNameNotMatched) {
    auto cfg = make_cfg_with_two();
    auto opts = build_model_picker_options(cfg, "(session:abc12345)");
    ASSERT_FALSE(opts.empty());
    for (const auto& o : opts) {
        EXPECT_FALSE(o.is_current) << "row " << o.name << " unexpectedly is_current";
    }
}

// 场景:空 saved_models + 任意 current_name → 仍至少返回一条合成 (legacy)。
// 这是新用户(没跑过 configure)第一次打 /model 的入口情形;picker 必须
// 至少有一行可点,否则用户卡在空列表里只能 Esc。
TEST(ModelPicker, EmptySavedModelsStillReturnsLegacy) {
    AppConfig cfg;
    cfg.provider = "copilot";
    cfg.copilot.model = "gpt-4o";
    auto opts = build_model_picker_options(cfg, "(legacy)");
    ASSERT_EQ(opts.size(), 1u);
    EXPECT_EQ(opts[0].name, "(legacy)");
    EXPECT_TRUE(opts[0].is_current);
}
