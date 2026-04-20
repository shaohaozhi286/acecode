// 覆盖 src/utils/models_dev_catalog.{hpp,cpp}：
// - build_catalog 把 api.json 的字段（context、cost、capabilities、modalities）
//   翻译成 ProviderEntry / ModelEntry，缺失字段走 nullopt 而不是抛异常。
// - openai_compatible 由 api.openai.base 字段触发。
// - format_context / format_cost / format_capabilities 的输出字符串。
// - 模型按 id 字母升序。

#include <gtest/gtest.h>

#include "utils/models_dev_catalog.hpp"

#include <nlohmann/json.hpp>

using namespace acecode;

namespace {

const char* kFixture = R"({
  "anthropic": {
    "name": "Anthropic",
    "env": ["ANTHROPIC_API_KEY"],
    "doc": "https://example.com/anthropic",
    "api": {"openai": {"base": "https://api.anthropic.com/v1"}},
    "models": {
      "claude-haiku": {"limit": {"context": 200000}, "cost": {"input": 1.0, "output": 5.0}, "tool_call": true, "attachment": true},
      "claude-sonnet": {"limit": {"context": 200000, "output": 64000}, "cost": {"input": 3, "output": 15}, "tool_call": true, "reasoning": true}
    }
  },
  "azure": {
    "name": "Azure",
    "env": ["AZURE_OPENAI_API_KEY"],
    "models": {}
  }
})";

} // namespace

// 场景：providers 数量、字段、capabilities 翻译都正确；缺失字段不崩。
TEST(ModelsDevCatalog, BuildFromFixture) {
    auto j = nlohmann::json::parse(kFixture);
    auto providers = build_catalog(j);
    ASSERT_EQ(providers.size(), 2u);

    const ProviderEntry* anthropic = nullptr;
    const ProviderEntry* azure = nullptr;
    for (const auto& p : providers) {
        if (p.id == "anthropic") anthropic = &p;
        else if (p.id == "azure") azure = &p;
    }
    ASSERT_NE(anthropic, nullptr);
    ASSERT_NE(azure, nullptr);

    EXPECT_TRUE(anthropic->openai_compatible);
    ASSERT_TRUE(anthropic->base_url.has_value());
    EXPECT_EQ(*anthropic->base_url, "https://api.anthropic.com/v1");
    EXPECT_FALSE(azure->openai_compatible);

    ASSERT_EQ(anthropic->models.size(), 2u);
    // models sorted by id ascending
    EXPECT_EQ(anthropic->models[0].id, "claude-haiku");
    EXPECT_EQ(anthropic->models[1].id, "claude-sonnet");

    const ModelEntry& haiku = anthropic->models[0];
    ASSERT_TRUE(haiku.context.has_value());
    EXPECT_EQ(*haiku.context, 200000);
    EXPECT_TRUE(haiku.tool_call);
    EXPECT_TRUE(haiku.attachment);
    EXPECT_FALSE(haiku.reasoning);
    ASSERT_TRUE(haiku.cost_input.has_value());
    EXPECT_DOUBLE_EQ(*haiku.cost_input, 1.0);

    const ModelEntry& sonnet = anthropic->models[1];
    EXPECT_TRUE(sonnet.reasoning);
    ASSERT_TRUE(sonnet.max_output.has_value());
    EXPECT_EQ(*sonnet.max_output, 64000);
}

// 场景：format_context 在常见量级上的友好输出。
TEST(ModelsDevCatalog, FormatContextRanges) {
    EXPECT_EQ(format_context(std::nullopt), "");
    EXPECT_EQ(format_context(0), "");
    EXPECT_EQ(format_context(800), "800");
    EXPECT_EQ(format_context(128000), "128k");
    EXPECT_EQ(format_context(200000), "200k");
    EXPECT_EQ(format_context(1000000), "1M");
}

// 场景：format_cost 处理只有一边 cost 的情况，不能塌陷。
TEST(ModelsDevCatalog, FormatCostHandlesPartial) {
    EXPECT_EQ(format_cost(std::nullopt, std::nullopt), "");
    EXPECT_EQ(format_cost(3.0, 15.0), "in=$3/out=$15");
    EXPECT_EQ(format_cost(0.27, 1.10), "in=$0.27/out=$1.1");
    EXPECT_EQ(format_cost(std::nullopt, 5.0), "in=$?/out=$5");
}

// 场景：format_capabilities 列出 tools/vision/reasoning，无 capability 返回空。
TEST(ModelsDevCatalog, FormatCapabilities) {
    ModelEntry m;
    EXPECT_EQ(format_capabilities(m), "");
    m.tool_call = true;
    m.attachment = true;
    m.reasoning = true;
    EXPECT_EQ(format_capabilities(m), "[tools, vision, reasoning]");
    m.deprecated = true;
    EXPECT_EQ(format_capabilities(m), "[tools, vision, reasoning, deprecated]");
}

// 场景：build_catalog 在退化输入下不崩（顶层非 object 返回空）。
TEST(ModelsDevCatalog, BuildCatalogToleratesBadInput) {
    EXPECT_TRUE(build_catalog(nlohmann::json::array()).empty());
    EXPECT_TRUE(build_catalog(nlohmann::json("string")).empty());
    EXPECT_TRUE(build_catalog(nlohmann::json::object()).empty());
}
