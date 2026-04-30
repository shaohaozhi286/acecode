// 覆盖 src/web/handlers/models_handler.cpp。前端 model-picker 要靠这里
// 拼出 saved_models + 合成 (legacy) 行;一旦回归:
//   - list_models 漏 (legacy) 行 → 用户切不回 legacy 兜底
//   - find_model_by_name 大小写敏感配错 → POST 切换 400
//   - is_legacy 标记错位 → 前端把 (legacy) 行误开放编辑入口

#include <gtest/gtest.h>

#include "web/handlers/models_handler.hpp"

#include "config/config.hpp"
#include "config/saved_models.hpp"

using acecode::AppConfig;
using acecode::ModelProfile;
using acecode::web::find_model_by_name;
using acecode::web::list_models;

namespace {

// 构造一个最小 cfg,带两个 saved_models 条目 + 走 copilot 的 legacy 默认
AppConfig make_cfg_with_two() {
    AppConfig cfg;
    cfg.provider = "copilot";
    cfg.copilot.model = "gpt-4o";

    ModelProfile a;
    a.name = "copilot-fast"; a.provider = "copilot"; a.model = "gpt-4o";
    cfg.saved_models.push_back(a);

    ModelProfile b;
    b.name = "local-lm"; b.provider = "openai"; b.model = "llama-3";
    b.base_url = "http://localhost:1234/v1"; b.api_key = "x";
    cfg.saved_models.push_back(b);

    return cfg;
}

} // namespace

// 场景: list_models 输出顺序 = saved_models 顺序 + 末尾追加 (legacy)。
TEST(ModelsHandler, ListIncludesAllSavedAndLegacyAtEnd) {
    auto cfg = make_cfg_with_two();
    auto arr = list_models(cfg);
    ASSERT_TRUE(arr.is_array());
    ASSERT_EQ(arr.size(), 3u); // 2 saved + 1 legacy

    EXPECT_EQ(arr[0]["name"], "copilot-fast");
    EXPECT_EQ(arr[0]["is_legacy"], false);
    EXPECT_EQ(arr[1]["name"], "local-lm");
    EXPECT_EQ(arr[1]["is_legacy"], false);
    EXPECT_TRUE(arr[1].contains("base_url"));

    EXPECT_EQ(arr[2]["name"], "(legacy)");
    EXPECT_EQ(arr[2]["is_legacy"], true);
    EXPECT_EQ(arr[2]["provider"], "copilot");
    EXPECT_EQ(arr[2]["model"], "gpt-4o");
}

// 场景: 空 saved_models 时,list_models 仍至少有一行 (legacy)。
TEST(ModelsHandler, ListEmptySavedStillReturnsLegacy) {
    AppConfig cfg;
    cfg.provider = "copilot";
    cfg.copilot.model = "gpt-3.5-turbo";
    auto arr = list_models(cfg);
    ASSERT_EQ(arr.size(), 1u);
    EXPECT_EQ(arr[0]["name"], "(legacy)");
    EXPECT_EQ(arr[0]["model"], "gpt-3.5-turbo");
}

// 场景: find_model_by_name 命中 saved_models 条目。
TEST(ModelsHandler, FindBySavedName) {
    auto cfg = make_cfg_with_two();
    auto e = find_model_by_name(cfg, "local-lm");
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->provider, "openai");
    EXPECT_EQ(e->model, "llama-3");
}

// 场景: find_model_by_name "(legacy)" 命中合成 entry。
TEST(ModelsHandler, FindLegacy) {
    auto cfg = make_cfg_with_two();
    auto e = find_model_by_name(cfg, "(legacy)");
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(e->provider, "copilot");
}

// 场景: 未命中 → nullopt。Caller 转 400。
TEST(ModelsHandler, FindUnknownReturnsNullopt) {
    auto cfg = make_cfg_with_two();
    EXPECT_FALSE(find_model_by_name(cfg, "nonexistent").has_value());
    EXPECT_FALSE(find_model_by_name(cfg, "").has_value());
}

// 场景: name 大小写敏感 — TUI /model 也是大小写敏感,必须保持一致。
TEST(ModelsHandler, FindIsCaseSensitive) {
    auto cfg = make_cfg_with_two();
    EXPECT_TRUE(find_model_by_name(cfg, "copilot-fast").has_value());
    EXPECT_FALSE(find_model_by_name(cfg, "COPILOT-FAST").has_value());
    EXPECT_FALSE(find_model_by_name(cfg, "Copilot-Fast").has_value());
}
