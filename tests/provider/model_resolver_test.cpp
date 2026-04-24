// 覆盖 src/provider/model_resolver.{hpp,cpp} 的纯函数 resolve_effective_model
// 与 synth_legacy_entry。对应 openspec/changes/model-profiles 的任务 7.8-7.14。
// 文件头与每个 TEST 都加中文注释,遵循 feedback_unit_test_chinese_comments 约定。

#include <gtest/gtest.h>

#include "provider/model_resolver.hpp"
#include "config/config.hpp"
#include "session/session_storage.hpp"

using namespace acecode;

namespace {

// 构造一个最小可用 AppConfig:legacy 是 copilot,saved_models 含两个 entry。
AppConfig make_cfg(const std::string& default_name = "") {
    AppConfig cfg;
    cfg.provider = "copilot";  // legacy 默认
    cfg.copilot.model = "gpt-4o-legacy";
    cfg.openai.base_url = "http://legacy.local/v1";
    cfg.openai.api_key = "legacy-key";
    cfg.openai.model = "legacy-openai-model";

    ModelProfile a;
    a.name = "alpha";
    a.provider = "openai";
    a.base_url = "http://alpha.local/v1";
    a.api_key = "ak";
    a.model = "alpha-model";
    cfg.saved_models.push_back(a);

    ModelProfile b;
    b.name = "beta";
    b.provider = "copilot";
    b.model = "gpt-4o-beta";
    cfg.saved_models.push_back(b);

    cfg.default_model_name = default_name;
    return cfg;
}

} // namespace

// 7.9 — 仅 default,无 cwd override / 无 resume → 返回 default 对应 entry。
TEST(ModelResolverTest, DefaultOnlyReturnsDefault) {
    AppConfig cfg = make_cfg("alpha");
    ModelProfile got = resolve_effective_model(cfg, std::nullopt, std::nullopt);
    EXPECT_EQ(got.name, "alpha");
    EXPECT_EQ(got.provider, "openai");
    EXPECT_EQ(got.model, "alpha-model");
}

// 7.10 — default + cwd override → 返回 cwd 指向的 entry。
TEST(ModelResolverTest, CwdOverrideBeatsDefault) {
    AppConfig cfg = make_cfg("alpha");
    auto override_name = std::optional<std::string>{"beta"};
    ModelProfile got = resolve_effective_model(cfg, override_name, std::nullopt);
    EXPECT_EQ(got.name, "beta");
    EXPECT_EQ(got.provider, "copilot");
}

// 7.11 — default + cwd + resume session(meta 命中 saved_models)→ session 优先,
// 即使 cwd override 与 default 都是别的 entry。
TEST(ModelResolverTest, ResumeMetaWinsOverCwdAndDefault) {
    AppConfig cfg = make_cfg("alpha");
    SessionMeta meta;
    meta.id = "abc12345-xx";
    meta.provider = "copilot";
    meta.model = "gpt-4o-beta";  // 应命中 saved_models 中 "beta"
    auto override_name = std::optional<std::string>{"alpha"};
    ModelProfile got = resolve_effective_model(cfg, override_name, std::optional{meta});
    EXPECT_EQ(got.name, "beta");
}

// 7.12 — saved_models 空 + default 空 → 返回 (legacy) 兜底 entry,
// provider/model 应来自 legacy 字段。
TEST(ModelResolverTest, EmptyConfigFallsBackToLegacy) {
    AppConfig cfg;  // 全部 default,saved_models 为空
    cfg.provider = "copilot";
    cfg.copilot.model = "legacy-cop";

    ModelProfile got = resolve_effective_model(cfg, std::nullopt, std::nullopt);
    EXPECT_EQ(got.name, "(legacy)");
    EXPECT_EQ(got.provider, "copilot");
    EXPECT_EQ(got.model, "legacy-cop");
}

// 7.13 — cwd override 指向已删 entry(saved_models 中不存在该 name)→
// resolver 不抛,降级到 default,然后到 legacy。
TEST(ModelResolverTest, MissingCwdOverrideFallsBack) {
    AppConfig cfg = make_cfg("alpha");
    auto override_name = std::optional<std::string>{"deleted-entry"};
    ModelProfile got = resolve_effective_model(cfg, override_name, std::nullopt);
    EXPECT_EQ(got.name, "alpha");  // 应降级到 default
}

// 7.14 — resume meta 不匹配 saved_models 也不匹配 legacy → 构造 ad-hoc entry,
// name 以 "(session:" 开头,字段从 meta + cfg.openai 借。
TEST(ModelResolverTest, UnmatchedResumeBuildsAdHocEntry) {
    AppConfig cfg = make_cfg("alpha");
    SessionMeta meta;
    meta.id = "9f2a1c3d-deadbeef";
    meta.provider = "openai";
    meta.model = "ghost-model";  // saved_models 与 legacy 都没有
    ModelProfile got = resolve_effective_model(cfg, std::nullopt, std::optional{meta});
    EXPECT_EQ(got.name.rfind("(session:", 0), 0u);
    EXPECT_EQ(got.provider, "openai");
    EXPECT_EQ(got.model, "ghost-model");
    // best-effort 借 cfg.openai.base_url / api_key。
    EXPECT_EQ(got.base_url, cfg.openai.base_url);
    EXPECT_EQ(got.api_key, cfg.openai.api_key);
}

// 额外 — synth_legacy_entry 对 openai legacy 提取所有字段。
TEST(ModelResolverTest, SynthLegacyEntryOpenaiCarriesAllFields) {
    AppConfig cfg;
    cfg.provider = "openai";
    cfg.openai.base_url = "http://oai.local/v1";
    cfg.openai.api_key = "sk-x";
    cfg.openai.model = "my-model";
    cfg.openai.models_dev_provider_id = "openrouter";

    ModelProfile got = synth_legacy_entry(cfg);
    EXPECT_EQ(got.name, "(legacy)");
    EXPECT_EQ(got.provider, "openai");
    EXPECT_EQ(got.base_url, "http://oai.local/v1");
    EXPECT_EQ(got.api_key, "sk-x");
    EXPECT_EQ(got.model, "my-model");
    ASSERT_TRUE(got.models_dev_provider_id.has_value());
    EXPECT_EQ(*got.models_dev_provider_id, "openrouter");
}

// 额外 — chosen_name == "(legacy)" 时 resolver 直接走 synth_legacy_entry。
TEST(ModelResolverTest, ExplicitLegacyChosenName) {
    AppConfig cfg = make_cfg("");  // default 空
    auto override_name = std::optional<std::string>{"(legacy)"};
    ModelProfile got = resolve_effective_model(cfg, override_name, std::nullopt);
    EXPECT_EQ(got.name, "(legacy)");
}
