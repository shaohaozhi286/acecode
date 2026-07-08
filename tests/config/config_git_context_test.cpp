// 覆盖 src/config/config.cpp 中 git_context 段的解析与 clamp 行为
// (openspec add-git-context):
//   - enabled     默认 true,false = 不采集/不注入快照,/api/git/* 按非仓库处理
//   - timeout_ms  默认 3000,有效区间 [500, 30000],越界静默 clamp(不 fatal,
//                 该值只影响 best-effort 采集,不值得阻塞启动)
//
// 与 config_proxy_probe_test 同款做法:复刻 load_config 的字段分支独立验证,
// 不依赖真实 config.json(load_config 直连用户目录且越界字段可能 exit)。

#include <gtest/gtest.h>

#include "config/config.hpp"

#include <nlohmann/json.hpp>

using namespace acecode;

namespace {

// 复刻 config.cpp::load_config 对 git_context 字段的解析 + clamp 逻辑。
void apply_git_context_fields(const nlohmann::json& gcj, GitContextConfig& cfg) {
    if (gcj.contains("enabled") && gcj["enabled"].is_boolean())
        cfg.enabled = gcj["enabled"].get<bool>();
    if (gcj.contains("timeout_ms") && gcj["timeout_ms"].is_number_integer()) {
        int v = gcj["timeout_ms"].get<int>();
        if (v < 500) v = 500;
        if (v > 30000) v = 30000;
        cfg.timeout_ms = v;
    }
}

} // namespace

// 场景:结构体默认值(config.json 无 git_context 段的机器)。
// 期望:enabled=true、timeout 3000 —— git 感知默认开箱即用。
TEST(ConfigGitContextDefaults, StructDefault) {
    GitContextConfig cfg;
    EXPECT_TRUE(cfg.enabled);
    EXPECT_EQ(cfg.timeout_ms, 3000);
    AppConfig app;
    EXPECT_TRUE(app.git_context.enabled);
}

// 场景:用户显式关闭 git 感知。
// 期望:enabled=false 生效。
TEST(ConfigGitContextLoader, DisabledPassesThrough) {
    GitContextConfig cfg;
    apply_git_context_fields(nlohmann::json{{"enabled", false}}, cfg);
    EXPECT_FALSE(cfg.enabled);
}

// 场景:timeout 在有效区间内 / 越界两侧。
// 期望:区间内原样;过小 clamp 到 500,过大 clamp 到 30000(不崩不退)。
TEST(ConfigGitContextLoader, TimeoutClamping) {
    GitContextConfig cfg;
    apply_git_context_fields(nlohmann::json{{"timeout_ms", 5000}}, cfg);
    EXPECT_EQ(cfg.timeout_ms, 5000);
    apply_git_context_fields(nlohmann::json{{"timeout_ms", 50}}, cfg);
    EXPECT_EQ(cfg.timeout_ms, 500);
    apply_git_context_fields(nlohmann::json{{"timeout_ms", 999999}}, cfg);
    EXPECT_EQ(cfg.timeout_ms, 30000);
}

// 场景:save_config 的稀疏写盘原则 —— 默认值不落盘。
// 期望:默认配置序列化后不出现 git_context 键;改动过的字段才写。
// (这里按 config.cpp 的写盘分支同款逻辑构造 json,验证判等条件成立。)
TEST(ConfigGitContextSave, SparseWrite) {
    GitContextConfig defaults;
    GitContextConfig changed;
    changed.enabled = false;
    changed.timeout_ms = 10000;

    nlohmann::json unchanged_json = nlohmann::json::object();
    if (defaults.enabled != GitContextConfig{}.enabled)
        unchanged_json["enabled"] = defaults.enabled;
    if (defaults.timeout_ms != GitContextConfig{}.timeout_ms)
        unchanged_json["timeout_ms"] = defaults.timeout_ms;
    EXPECT_TRUE(unchanged_json.empty());

    nlohmann::json changed_json = nlohmann::json::object();
    if (changed.enabled != GitContextConfig{}.enabled)
        changed_json["enabled"] = changed.enabled;
    if (changed.timeout_ms != GitContextConfig{}.timeout_ms)
        changed_json["timeout_ms"] = changed.timeout_ms;
    EXPECT_EQ(changed_json.size(), 2u);
}
