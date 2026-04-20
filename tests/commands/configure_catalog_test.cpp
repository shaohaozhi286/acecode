// 覆盖 src/commands/configure_catalog.cpp 的纯函数部分（不触发交互式 stdin）：
// - filter_providers / filter_models 的子串过滤行为
// - format_provider_row / format_model_row / format_model_summary 的渲染输出
// - format_source_line 三种状态
// - lookup_env_key 在命中和缺失时的返回值
//
// 交互式选择函数（run_provider_picker / run_model_picker）需要 stdin TTY，
// 不在单元测试覆盖范围内；它们的逻辑通过这里的 helper 间接保证正确。

#include <gtest/gtest.h>

#include "commands/configure_catalog.hpp"

#include <cstdlib>

using namespace acecode;

namespace {

#ifdef _WIN32
void set_env(const char* k, const char* v) { _putenv_s(k, v ? v : ""); }
#else
void set_env(const char* k, const char* v) {
    if (v) ::setenv(k, v, 1);
    else ::unsetenv(k);
}
#endif

ProviderEntry make_provider(const std::string& id,
                            const std::string& name,
                            std::vector<std::string> env = {}) {
    ProviderEntry p;
    p.id = id;
    p.name = name;
    p.env = std::move(env);
    return p;
}

} // namespace

// 场景：filter_providers 大小写不敏感；空查询返回原列表。
TEST(ConfigureCatalog, FilterProvidersSubstring) {
    ProviderEntry a = make_provider("anthropic", "Anthropic");
    ProviderEntry b = make_provider("openrouter", "OpenRouter");
    ProviderEntry c = make_provider("openai", "OpenAI");
    std::vector<const ProviderEntry*> src{&a, &b, &c};

    EXPECT_EQ(filter_providers(src, "").size(), 3u);
    EXPECT_EQ(filter_providers(src, "OPEN").size(), 2u);
    auto only_anth = filter_providers(src, "thro");
    ASSERT_EQ(only_anth.size(), 1u);
    EXPECT_EQ(only_anth[0]->id, "anthropic");
    EXPECT_EQ(filter_providers(src, "no-such-provider").size(), 0u);
}

// 场景：filter_models 仅按 id 子串匹配，不看 name。
TEST(ConfigureCatalog, FilterModelsByIdOnly) {
    ModelEntry x; x.id = "claude-haiku"; x.name = "Claude Haiku";
    ModelEntry y; y.id = "claude-sonnet"; y.name = "Claude Sonnet";
    ModelEntry z; z.id = "gpt-4o"; z.name = "GPT-4o";
    std::vector<const ModelEntry*> src{&x, &y, &z};

    EXPECT_EQ(filter_models(src, "haiku").size(), 1u);
    EXPECT_EQ(filter_models(src, "claude").size(), 2u);
    EXPECT_EQ(filter_models(src, "GPT").size(), 1u);
    EXPECT_EQ(filter_models(src, "Claude Haiku").size(), 0u);  // 'name' is ignored
}

// 场景：format_provider_row 含 id、模型数、doc；name 与 id 不同时附 (name)。
TEST(ConfigureCatalog, FormatProviderRow) {
    ProviderEntry p = make_provider("openrouter", "OpenRouter");
    p.doc = "https://docs";
    p.models.resize(42);
    auto row = format_provider_row(p);
    EXPECT_NE(row.find("openrouter"), std::string::npos);
    EXPECT_NE(row.find("(OpenRouter)"), std::string::npos);
    EXPECT_NE(row.find("models=42"), std::string::npos);
    EXPECT_NE(row.find("doc=https://docs"), std::string::npos);
}

// 场景：format_model_row 缺字段时不输出对应段。
TEST(ConfigureCatalog, FormatModelRowOmitsMissingFields) {
    ModelEntry m;
    m.id = "bare-model";
    auto row = format_model_row(m);
    EXPECT_EQ(row, "bare-model");

    m.context = 200000;
    m.cost_input = 3.0;
    m.cost_output = 15.0;
    m.tool_call = true;
    auto row2 = format_model_row(m);
    EXPECT_NE(row2.find("ctx=200k"), std::string::npos);
    EXPECT_NE(row2.find("in=$3/out=$15"), std::string::npos);
    EXPECT_NE(row2.find("[tools]"), std::string::npos);
}

// 场景：format_model_summary 多行输出，含 capabilities/knowledge。
TEST(ConfigureCatalog, FormatModelSummary) {
    ModelEntry m;
    m.id = "claude-sonnet-4-5";
    m.name = "Claude Sonnet 4.5";
    m.context = 200000;
    m.max_output = 64000;
    m.cost_input = 3.0;
    m.cost_output = 15.0;
    m.tool_call = true;
    m.attachment = true;
    m.reasoning = true;
    m.input_modalities = {"text", "image"};
    m.knowledge_cutoff = "2025-02-28";
    auto s = format_model_summary(m);
    EXPECT_NE(s.find("Selected model: claude-sonnet-4-5"), std::string::npos);
    EXPECT_NE(s.find("(Claude Sonnet 4.5)"), std::string::npos);
    EXPECT_NE(s.find("context:       200k tokens"), std::string::npos);
    EXPECT_NE(s.find("max output:    64000"), std::string::npos);
    EXPECT_NE(s.find("[tools, vision, reasoning]"), std::string::npos);
    EXPECT_NE(s.find("text, image"), std::string::npos);
    EXPECT_NE(s.find("knowledge:     2025-02-28"), std::string::npos);
}

// 场景：format_source_line 区分 copilot / catalog / custom。
TEST(ConfigureCatalog, FormatSourceLineThreeStates) {
    AppConfig cfg;
    cfg.provider = "copilot";
    EXPECT_EQ(format_source_line(cfg), "copilot");

    cfg.provider = "openai";
    cfg.openai.models_dev_provider_id.reset();
    EXPECT_EQ(format_source_line(cfg), "openai (custom)");

    cfg.openai.models_dev_provider_id = "openrouter";
    EXPECT_EQ(format_source_line(cfg), "openai (provider=openrouter via models.dev)");
}

// 场景：lookup_env_key 命中第一个存在的 env 变量；都缺失返回 nullopt。
TEST(ConfigureCatalog, LookupEnvKey) {
    ProviderEntry p = make_provider("groq", "Groq",
                                    {"GROQ_API_KEY", "GROQ_TOKEN"});

    set_env("GROQ_API_KEY", nullptr);
    set_env("GROQ_TOKEN", nullptr);
    EXPECT_FALSE(lookup_env_key(p).has_value());

    set_env("GROQ_TOKEN", "fallback-secret");
    auto hit = lookup_env_key(p);
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->env_name, "GROQ_TOKEN");
    EXPECT_EQ(hit->value, "fallback-secret");

    set_env("GROQ_API_KEY", "primary-secret");
    auto hit2 = lookup_env_key(p);
    ASSERT_TRUE(hit2.has_value());
    EXPECT_EQ(hit2->env_name, "GROQ_API_KEY");

    set_env("GROQ_API_KEY", nullptr);
    set_env("GROQ_TOKEN", nullptr);
}
