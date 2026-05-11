// 覆盖 src/provider/model_resolver.{hpp,cpp} 的纯函数 resolve_effective_model。
// 对应 openspec/changes/model-profiles 的任务 7.8-7.14。
// 文件头与每个 TEST 都加中文注释,遵循 feedback_unit_test_chinese_comments 约定。

#include <gtest/gtest.h>

#include "provider/model_resolver.hpp"
#include "config/config.hpp"
#include "session/session_storage.hpp"

using namespace acecode;

namespace {

// 构造一个最小可用 AppConfig:saved_models 含两个 entry。
AppConfig make_cfg(const std::string& default_name = "") {
    AppConfig cfg;
    cfg.openai.base_url = "http://legacy.local/v1";
    cfg.openai.api_key = "legacy-key";

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

// 7.12 — saved_models 空 + default 空 → 从旧 schema 字段合成 copilot profile,
// 确保 daemon / desktop 在未迁移配置下也能启动。
TEST(ModelResolverTest, EmptyConfigUsesLegacyCopilotFallback) {
    AppConfig cfg;

    ModelProfile got = resolve_effective_model(cfg, std::nullopt, std::nullopt);
    EXPECT_EQ(got.name, "copilot");
    EXPECT_EQ(got.provider, "copilot");
    EXPECT_EQ(got.model, "gpt-4o");
}

// 额外 — saved_models 为空但旧 openai 字段可用时,直接用旧字段构造临时
// profile。api_key 允许为空,由 OpenAI-compatible provider 决定是否发送鉴权头。
TEST(ModelResolverTest, EmptyConfigUsesLegacyOpenAiFields) {
    AppConfig cfg;
    cfg.provider = "openai";
    cfg.openai.base_url = "http://localhost:1234/v1";
    cfg.openai.api_key = "";
    cfg.openai.model = "local-model";

    ModelProfile got = resolve_effective_model(cfg, std::nullopt, std::nullopt);
    EXPECT_EQ(got.name, "openai");
    EXPECT_EQ(got.provider, "openai");
    EXPECT_EQ(got.base_url, "http://localhost:1234/v1");
    EXPECT_EQ(got.api_key, "");
    EXPECT_EQ(got.model, "local-model");
}

// 7.13 — cwd override 指向已删 entry(saved_models 中不存在该 name)→
// resolver 不抛,降级到 default。
TEST(ModelResolverTest, MissingCwdOverrideFallsBack) {
    AppConfig cfg = make_cfg("alpha");
    auto override_name = std::optional<std::string>{"deleted-entry"};
    ModelProfile got = resolve_effective_model(cfg, override_name, std::nullopt);
    EXPECT_EQ(got.name, "alpha");  // 应降级到 default
}

// 7.14 — resume meta 不匹配 saved_models → 构造 ad-hoc entry,
// name 以 "(session:" 开头,字段从 meta + cfg.openai 借。
TEST(ModelResolverTest, UnmatchedResumeBuildsAdHocEntry) {
    AppConfig cfg = make_cfg("alpha");
    SessionMeta meta;
    meta.id = "9f2a1c3d-deadbeef";
    meta.provider = "openai";
    meta.model = "ghost-model";  // saved_models 没有
    ModelProfile got = resolve_effective_model(cfg, std::nullopt, std::optional{meta});
    EXPECT_EQ(got.name.rfind("(session:", 0), 0u);
    EXPECT_EQ(got.provider, "openai");
    EXPECT_EQ(got.model, "ghost-model");
    // best-effort 借 cfg.openai.base_url / api_key。
    EXPECT_EQ(got.base_url, cfg.openai.base_url);
    EXPECT_EQ(got.api_key, cfg.openai.api_key);
}

// 额外 — default_model_name 为空但 saved_models 非空 → 取第一条 saved model。
TEST(ModelResolverTest, EmptyDefaultUsesFirstSavedModel) {
    AppConfig cfg = make_cfg("");
    ModelProfile got = resolve_effective_model(cfg, std::nullopt, std::nullopt);
    EXPECT_EQ(got.name, "alpha");
}
