// 覆盖 model_context_resolver 的非阻塞 session-facing 解析路径。
// 重点保护:session create/resume 不应为了远程 /models 元数据阻塞。

#include <gtest/gtest.h>

#include "config/config.hpp"
#include "config/saved_models.hpp"
#include "provider/model_context_resolver.hpp"
#include "provider/models_dev_registry.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path tmp_dir(const std::string& tag) {
    auto p = fs::temp_directory_path() / ("acecode_model_ctx_" + tag);
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

void write_file(const fs::path& p, const std::string& contents) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << contents;
}

acecode::AppConfig make_openrouter_cfg(const fs::path& registry_path) {
    acecode::AppConfig cfg;
    cfg.provider = "openai";
    cfg.openai.base_url = "http://127.0.0.1:9/v1";
    cfg.openai.api_key = "test-key";
    cfg.openai.model = "poolside/laguna-xs.2:free";
    cfg.openai.models_dev_provider_id = "openrouter";
    cfg.models_dev.user_override_path = registry_path.string();
    return cfg;
}

const char* kOpenRouterRegistry = R"({
  "openrouter": {
    "id": "openrouter",
    "models": {
      "poolside/laguna-xs.2:free": {
        "id": "poolside/laguna-xs.2:free",
        "limit": { "context": 131072 }
      }
    }
  }
})";

const char* kOtherRegistry = R"({
  "other": {
    "id": "other",
    "models": {
      "other-model": { "id": "other-model", "context_length": 4096 }
    }
  }
})";

} // namespace

// 场景:本地 models.dev 有匹配项 → 非阻塞解析直接返回准确 context。
TEST(ModelContextResolver, NonblockingUsesLocalModelsDevContext) {
    acecode::reset_model_context_window_cache_for_test();
    auto dir = tmp_dir("local");
    auto registry_path = dir / "api.json";
    write_file(registry_path, kOpenRouterRegistry);

    auto cfg = make_openrouter_cfg(registry_path);
    acecode::initialize_registry(cfg, "");

    int got = acecode::resolve_model_context_window_nonblocking(
        cfg, "openai", cfg.openai.model, 128000);

    EXPECT_EQ(got, 131072);
}

// 场景:首次命中本地 metadata 后写入进程缓存;随后 registry 丢失同 provider,
// 同一模型 key 仍从缓存返回,不退回 fallback。
TEST(ModelContextResolver, NonblockingReturnsProcessCacheBeforeFallback) {
    acecode::reset_model_context_window_cache_for_test();
    auto dir = tmp_dir("cache");
    auto registry_path = dir / "api.json";
    write_file(registry_path, kOpenRouterRegistry);

    auto cfg = make_openrouter_cfg(registry_path);
    acecode::initialize_registry(cfg, "");
    ASSERT_EQ(acecode::resolve_model_context_window_nonblocking(
                  cfg, "openai", cfg.openai.model, 128000),
              131072);

    auto other_path = dir / "other.json";
    write_file(other_path, kOtherRegistry);
    auto cfg_without_match = cfg;
    cfg_without_match.models_dev.user_override_path = other_path.string();
    acecode::initialize_registry(cfg_without_match, "");

    int got = acecode::resolve_model_context_window_nonblocking(
        cfg_without_match, "openai", cfg_without_match.openai.model, 128000);

    EXPECT_EQ(got, 131072);
}

// 场景:没有缓存/本地 metadata 且不能 probe endpoint → 立即返回 fallback。
TEST(ModelContextResolver, NonblockingFallsBackWithoutEndpointProbe) {
    acecode::reset_model_context_window_cache_for_test();
    auto dir = tmp_dir("fallback");
    auto registry_path = dir / "api.json";
    write_file(registry_path, kOtherRegistry);

    auto cfg = make_openrouter_cfg(registry_path);
    cfg.openai.base_url.clear();
    acecode::initialize_registry(cfg, "");

    int got = acecode::resolve_model_context_window_nonblocking(
        cfg, "openai", cfg.openai.model, 77777);

    EXPECT_EQ(got, 77777);
}

// 场景:Codex provider 使用 Codex CLI 模型 catalog 的运行上下文,不回退到全局 128k。
TEST(ModelContextResolver, NonblockingUsesCodexModelContext) {
    acecode::reset_model_context_window_cache_for_test();
    acecode::AppConfig cfg;
    cfg.provider = "codex";
    cfg.context_window = 128000;

    EXPECT_EQ(acecode::resolve_model_context_window_nonblocking(
                  cfg, "codex", "gpt-5.5", cfg.context_window),
              272000);
    EXPECT_EQ(acecode::resolve_model_context_window_nonblocking(
                  cfg, "codex", "gpt-5.3-codex-spark", cfg.context_window),
              128000);
}

// 场景:saved model 配了手动 context_window → 优先于 models.dev / cache / fallback。
TEST(ModelContextResolver, ProfileContextWindowOverrideWins) {
    acecode::reset_model_context_window_cache_for_test();
    auto dir = tmp_dir("profile_override");
    auto registry_path = dir / "api.json";
    write_file(registry_path, kOpenRouterRegistry);

    auto cfg = make_openrouter_cfg(registry_path);
    acecode::initialize_registry(cfg, "");

    acecode::ModelProfile profile;
    profile.name = "manual";
    profile.provider = "openai";
    profile.base_url = cfg.openai.base_url;
    profile.api_key = cfg.openai.api_key;
    profile.model = cfg.openai.model;
    profile.models_dev_provider_id = "openrouter";
    profile.context_window = 64000;

    EXPECT_EQ(acecode::resolve_model_profile_context_window_nonblocking(
                  cfg, profile, 128000),
              64000);
    EXPECT_EQ(acecode::resolve_model_profile_context_window(
                  cfg, profile, 128000),
              64000);
}

// 场景:model pool 已发现同 model 的窗口时,用户在 saved model 上填写的
// 手动值仍然是最终 runtime 预算;清空手动值后才采用 pool 窗口。
TEST(ModelContextResolver, RuntimeProfileOverrideWinsOverModelPoolWindow) {
    acecode::AppConfig cfg;
    cfg.context_window = 128000;

    acecode::ModelProfile profile;
    profile.name = "manual";
    profile.provider = "copilot";
    profile.model = "pool-model";
    profile.context_window = 64000;

    EXPECT_EQ(acecode::resolve_runtime_model_profile_context_window_nonblocking(
                  cfg, profile, cfg.context_window, 120000),
              64000);

    profile.context_window.reset();
    EXPECT_EQ(acecode::resolve_runtime_model_profile_context_window_nonblocking(
                  cfg, profile, cfg.context_window, 120000),
              120000);
}
