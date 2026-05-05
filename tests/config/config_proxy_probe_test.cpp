// 覆盖 src/config/config.cpp 中 network 段对 `proxy_probe_enabled` /
// `proxy_probe_timeout_ms` 的解析与 clamp 行为。
//
// openspec/changes/proxy-fallback-on-unreachable 引入这两个字段:
//   - proxy_probe_enabled  默认 true,false = 完全跳过启动 TCP probe
//   - proxy_probe_timeout_ms 默认 1500,有效区间 [200, 10000],越界 clamp 到边界
//
// 复刻 load_config 中 network 段的相关分支以独立验证 clamp 行为,不依赖真实
// config.json。

#include <gtest/gtest.h>

#include "config/config.hpp"

#include <nlohmann/json.hpp>

using namespace acecode;

namespace {

// 复刻 config.cpp::load_config 对 network.proxy_probe_* 字段的解析 + clamp 逻辑。
// 返回 (clamped) timeout 实际写入值;cfg 是 in/out 参数。
int apply_probe_fields(const nlohmann::json& nj, NetworkConfig& cfg) {
    if (nj.contains("proxy_probe_enabled") && nj["proxy_probe_enabled"].is_boolean())
        cfg.proxy_probe_enabled = nj["proxy_probe_enabled"].get<bool>();
    if (nj.contains("proxy_probe_timeout_ms") &&
        nj["proxy_probe_timeout_ms"].is_number_integer()) {
        int raw = nj["proxy_probe_timeout_ms"].get<int>();
        int clamped = raw;
        if (clamped < 200)   clamped = 200;
        if (clamped > 10000) clamped = 10000;
        cfg.proxy_probe_timeout_ms = clamped;
    }
    return cfg.proxy_probe_timeout_ms;
}

} // namespace

// 场景:NetworkConfig 默认值 — probe 默认开,timeout 1500ms
TEST(ConfigProxyProbeDefaults, StructDefault) {
    NetworkConfig n;
    EXPECT_TRUE(n.proxy_probe_enabled);
    EXPECT_EQ(n.proxy_probe_timeout_ms, 1500);
}

// 场景:无 network 段时全是默认值
TEST(ConfigProxyProbeDefaults, NestedInAppConfig) {
    AppConfig cfg;
    EXPECT_TRUE(cfg.network.proxy_probe_enabled);
    EXPECT_EQ(cfg.network.proxy_probe_timeout_ms, 1500);
}

// 场景:enabled=false 透传
TEST(ConfigProxyProbeLoader, EnabledFalsePassesThrough) {
    NetworkConfig cfg;
    nlohmann::json nj = {{"proxy_probe_enabled", false}};
    apply_probe_fields(nj, cfg);
    EXPECT_FALSE(cfg.proxy_probe_enabled);
    EXPECT_EQ(cfg.proxy_probe_timeout_ms, 1500); // 未设 timeout 时保持默认
}

// 场景:合法 timeout 透传(等于下限 200)
TEST(ConfigProxyProbeLoader, TimeoutAtLowerBoundPassesThrough) {
    NetworkConfig cfg;
    nlohmann::json nj = {{"proxy_probe_timeout_ms", 200}};
    EXPECT_EQ(apply_probe_fields(nj, cfg), 200);
}

// 场景:合法 timeout 透传(等于上限 10000)
TEST(ConfigProxyProbeLoader, TimeoutAtUpperBoundPassesThrough) {
    NetworkConfig cfg;
    nlohmann::json nj = {{"proxy_probe_timeout_ms", 10000}};
    EXPECT_EQ(apply_probe_fields(nj, cfg), 10000);
}

// 场景:timeout 小于下限 → clamp 到 200
TEST(ConfigProxyProbeLoader, TimeoutTooSmallClampsToLowerBound) {
    NetworkConfig cfg;
    nlohmann::json nj = {{"proxy_probe_timeout_ms", 50}};
    EXPECT_EQ(apply_probe_fields(nj, cfg), 200);
}

// 场景:timeout 大于上限 → clamp 到 10000
TEST(ConfigProxyProbeLoader, TimeoutTooLargeClampsToUpperBound) {
    NetworkConfig cfg;
    nlohmann::json nj = {{"proxy_probe_timeout_ms", 99999}};
    EXPECT_EQ(apply_probe_fields(nj, cfg), 10000);
}

// 场景:timeout=0 / 负数 也 clamp 到 200(防御性)
TEST(ConfigProxyProbeLoader, TimeoutZeroOrNegativeClampsToLowerBound) {
    NetworkConfig cfg;
    nlohmann::json nj1 = {{"proxy_probe_timeout_ms", 0}};
    EXPECT_EQ(apply_probe_fields(nj1, cfg), 200);
    NetworkConfig cfg2;
    nlohmann::json nj2 = {{"proxy_probe_timeout_ms", -100}};
    EXPECT_EQ(apply_probe_fields(nj2, cfg2), 200);
}

// 场景:同时设 enabled + timeout
TEST(ConfigProxyProbeLoader, BothFieldsSetTogether) {
    NetworkConfig cfg;
    nlohmann::json nj = {
        {"proxy_probe_enabled", false},
        {"proxy_probe_timeout_ms", 800},
    };
    apply_probe_fields(nj, cfg);
    EXPECT_FALSE(cfg.proxy_probe_enabled);
    EXPECT_EQ(cfg.proxy_probe_timeout_ms, 800);
}
