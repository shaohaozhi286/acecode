// tests/tui/model_picker_test.cpp
//
// 覆盖 src/tui/model_picker.cpp 的 build_model_picker_options 纯函数。
// 任一回归(顺序错、is_current 标记错位)都会让 picker UI 反映错误的
// 当前模型 / 漏选项,所以这几条 case 都是 picker 正确性的最小护栏。
//
// 触发场景 / 期望(每个 TEST 子句):
//   - current_name 命中 saved_models 某条 → is_current 标记
//   - current_name 不命中任何条目 → 全部 is_current=false
//   - 空 saved_models → 返回空列表

#include "tui/model_picker.hpp"

#include "config/config.hpp"
#include "config/saved_models.hpp"

#include <gtest/gtest.h>

using acecode::AppConfig;
using acecode::build_model_picker_options;
using acecode::ModelPickerOption;
using acecode::ModelProfile;

namespace {

// 构造一份典型的 cfg:saved_models 里两条:copilot-fast / local-lm。
AppConfig make_cfg_with_two() {
    AppConfig cfg;
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

// 场景:current_name 命中 saved_models 里某条 → 该条 is_current=true。
TEST(ModelPicker, BuildOptionsMarksCurrent) {
    auto cfg = make_cfg_with_two();
    auto opts = build_model_picker_options(cfg, "copilot-fast");
    ASSERT_EQ(opts.size(), 2u);
    EXPECT_EQ(opts[0].name, "copilot-fast");
    EXPECT_TRUE(opts[0].is_current);
    EXPECT_EQ(opts[1].name, "local-lm");
    EXPECT_FALSE(opts[1].is_current);
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

// 场景:空 saved_models + 任意 current_name → 返回空列表。
TEST(ModelPicker, EmptySavedModelsReturnsEmptyList) {
    AppConfig cfg;
    auto opts = build_model_picker_options(cfg, "copilot-fast");
    EXPECT_TRUE(opts.empty());
}

// 场景:旧配置里还有 codex saved model → picker 不展示已屏蔽 provider。
TEST(ModelPicker, BuildOptionsSkipsDisabledCodexProvider) {
    auto cfg = make_cfg_with_two();
    ModelProfile c;
    c.name = "codex";
    c.provider = "codex";
    c.model = "gpt-5.5";
    cfg.saved_models.push_back(c);

    auto opts = build_model_picker_options(cfg, "codex");

    ASSERT_EQ(opts.size(), 2u);
    for (const auto& o : opts) {
        EXPECT_NE(o.provider, "codex");
        EXPECT_FALSE(o.is_current);
    }
}
