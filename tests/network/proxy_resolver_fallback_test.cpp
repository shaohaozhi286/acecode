// 覆盖 src/network/proxy_resolver.cpp 中由 openspec/changes/proxy-fallback-on-unreachable
// 引入的 probe_and_maybe_fallback / fallback_active_ / refresh 行为。
//
// 用 set_tcp_probe_for_testing 注入桩函数,不依赖真实网络。每个 TEST 顶部一行
// 中文注释指向对应 spec scenario:
//   - probe Ok → flag 不变
//   - probe Refused → flag 设、effective() 返回 auto-fallback
//   - probe_enabled=false → 跳过 probe(等价旧行为)
//   - refresh() 清 flag 并重探
//   - options_for 使用前会 lazy probe,代理不通就 fallback
//   - /proxy off 仍胜出;set 会清旧 fallback,但新代理使用前仍要探测
//   - /proxy reset 在 fallback 仍激活时回 auto-fallback,而非原 URL

#include <gtest/gtest.h>

#include "network/proxy_resolver.hpp"
#include "network/tcp_probe.hpp"
#include "config/config.hpp"

#include <atomic>

using acecode::network::ProxyResolver;
using acecode::network::TcpProbeReason;
using acecode::network::TcpProbeResult;
using acecode::NetworkConfig;

namespace {

// 在 ctor/dtor 安装/卸载桩,防止跨 TEST 污染全局状态。
class StubProbe {
public:
    explicit StubProbe(TcpProbeResult res)
        : res_(std::move(res)) {
        acecode::network::set_tcp_probe_for_testing(
            [r = res_](const std::string&, int, int) { return r; });
    }
    ~StubProbe() {
        acecode::network::set_tcp_probe_for_testing(nullptr);
    }
private:
    TcpProbeResult res_;
};

NetworkConfig make_manual_cfg(const std::string& proxy_url, bool probe_enabled = true) {
    NetworkConfig n;
    n.proxy_mode = "manual";
    n.proxy_url = proxy_url;
    n.proxy_probe_enabled = probe_enabled;
    n.proxy_probe_timeout_ms = 200;
    return n;
}

} // namespace

// 场景:probe 返回 Ok → fallback flag 不被设
TEST(ProxyResolverFallback, ProbeOkLeavesFlagFalse) {
    StubProbe stub({TcpProbeReason::Ok, ""});
    ProxyResolver r;
    r.init(make_manual_cfg("http://127.0.0.1:8888"));
    r.probe_and_maybe_fallback();

    EXPECT_FALSE(r.is_fallback_active());
    auto eff = r.effective("https://api.example.com");
    EXPECT_EQ(eff.url, "http://127.0.0.1:8888");
    EXPECT_EQ(eff.source, "manual");
}

// 场景:probe 返回 Refused → fallback 激活,effective() 返回 auto-fallback / 直连
TEST(ProxyResolverFallback, ProbeRefusedActivatesFallback) {
    StubProbe stub({TcpProbeReason::Refused, ""});
    ProxyResolver r;
    r.init(make_manual_cfg("http://127.0.0.1:8888"));
    r.probe_and_maybe_fallback();

    EXPECT_TRUE(r.is_fallback_active());
    auto eff = r.effective("https://api.example.com");
    EXPECT_EQ(eff.url, "");
    EXPECT_EQ(eff.source, "auto-fallback");

    auto info = r.fallback_info_snapshot();
    EXPECT_TRUE(info.active);
    EXPECT_EQ(info.reason, "Refused");
    EXPECT_EQ(info.original_source, "manual");
    EXPECT_NE(info.original_url.find("127.0.0.1:8888"), std::string::npos);
}

// 场景:probe Timeout 也会激活,reason 字段反映出来
TEST(ProxyResolverFallback, ProbeTimeoutActivatesWithCorrectReason) {
    StubProbe stub({TcpProbeReason::Timeout, ""});
    ProxyResolver r;
    r.init(make_manual_cfg("http://10.255.255.1:1"));
    r.probe_and_maybe_fallback();

    EXPECT_TRUE(r.is_fallback_active());
    EXPECT_EQ(r.fallback_info_snapshot().reason, "Timeout");
}

// 场景:probe_enabled=false → 完全跳过探测,即使代理不可达也不激活 fallback
TEST(ProxyResolverFallback, ProbeDisabledSkipsProbe) {
    StubProbe stub({TcpProbeReason::Refused, ""});
    ProxyResolver r;
    r.init(make_manual_cfg("http://127.0.0.1:8888", /*probe_enabled=*/false));
    r.probe_and_maybe_fallback();

    EXPECT_FALSE(r.is_fallback_active());
    auto eff = r.effective("https://api.example.com");
    EXPECT_EQ(eff.source, "manual"); // 维持原行为
}

// 场景:refresh() 清除 fallback 并重新探测;现在 probe 返回 Ok 应当回到正常状态
TEST(ProxyResolverFallback, RefreshReprobesAndClearsOnRecovery) {
    ProxyResolver r;
    r.init(make_manual_cfg("http://127.0.0.1:8888"));

    // 第一次探测:Refused → fallback 激活
    {
        StubProbe stub({TcpProbeReason::Refused, ""});
        r.probe_and_maybe_fallback();
    }
    ASSERT_TRUE(r.is_fallback_active());

    // 启动 listener 后 refresh:第二次探测 Ok → fallback 清除
    {
        StubProbe stub({TcpProbeReason::Ok, ""});
        r.refresh();
    }
    EXPECT_FALSE(r.is_fallback_active());
    auto eff = r.effective("https://api.example.com");
    EXPECT_EQ(eff.url, "http://127.0.0.1:8888");
    EXPECT_EQ(eff.source, "manual");
}

// 场景:fallback 激活时 /proxy off (session override) 仍胜出
TEST(ProxyResolverFallback, SessionOverrideOffBeatsFallback) {
    StubProbe stub({TcpProbeReason::Refused, ""});
    ProxyResolver r;
    r.init(make_manual_cfg("http://127.0.0.1:8888"));
    r.probe_and_maybe_fallback();
    ASSERT_TRUE(r.is_fallback_active());

    r.set_session_override_off();
    auto eff = r.effective("https://api.example.com");
    // session override 优先级高于 fallback,source 应当是 session-override:off
    EXPECT_EQ(eff.source, "session-override:off");
    EXPECT_EQ(eff.url, "");
}

// 场景:fallback 激活时 /proxy set <url> 仍胜出(用户显式意志)
TEST(ProxyResolverFallback, SessionOverrideManualBeatsFallback) {
    StubProbe stub({TcpProbeReason::Refused, ""});
    ProxyResolver r;
    r.init(make_manual_cfg("http://127.0.0.1:8888"));
    r.probe_and_maybe_fallback();
    ASSERT_TRUE(r.is_fallback_active());

    r.set_session_override_url("http://otherproxy.local:3128");
    auto eff = r.effective("https://api.example.com");
    EXPECT_EQ(eff.source, "session-override");
    EXPECT_NE(eff.url.find("otherproxy.local"), std::string::npos);
}

// 场景:没有显式调用 probe_and_maybe_fallback,options_for 使用代理前也会探测;
// 代理 Refused 时直接返回 auto-fallback / 直连
TEST(ProxyResolverFallback, OptionsForManualProxyProbesAndFallsBack) {
    StubProbe stub({TcpProbeReason::Refused, ""});
    ProxyResolver r;
    r.init(make_manual_cfg("http://127.0.0.1:8888"));

    auto opts = r.options_for("https://api.example.com");
    EXPECT_TRUE(r.is_fallback_active());
    EXPECT_FALSE(opts.proxies.has("http"));
    EXPECT_FALSE(opts.proxies.has("https"));
    EXPECT_EQ(opts.resolved.source, "auto-fallback");
}

// 场景:/proxy set <url> 也不绕过使用前探测;新代理不通时同样 fallback
TEST(ProxyResolverFallback, OptionsForSessionOverrideProxyProbesAndFallsBack) {
    StubProbe stub({TcpProbeReason::Refused, ""});
    NetworkConfig n;
    n.proxy_mode = "off";
    n.proxy_probe_enabled = true;
    n.proxy_probe_timeout_ms = 200;
    ProxyResolver r;
    r.init(n);
    r.set_session_override_url("http://127.0.0.1:9999");

    auto opts = r.options_for("https://api.example.com");
    EXPECT_TRUE(r.is_fallback_active());
    EXPECT_FALSE(opts.proxies.has("http"));
    EXPECT_FALSE(opts.proxies.has("https"));
    EXPECT_EQ(opts.resolved.source, "auto-fallback");
    EXPECT_EQ(r.fallback_info_snapshot().original_source, "session-override");
}

// 场景:使用前探测 Ok 时仍正常返回代理参数
TEST(ProxyResolverFallback, OptionsForManualProxyKeepsReachableProxy) {
    StubProbe stub({TcpProbeReason::Ok, ""});
    ProxyResolver r;
    r.init(make_manual_cfg("http://127.0.0.1:8888"));

    auto opts = r.options_for("https://api.example.com");
    EXPECT_FALSE(r.is_fallback_active());
    EXPECT_TRUE(opts.proxies.has("http"));
    EXPECT_TRUE(opts.proxies.has("https"));
    EXPECT_EQ(opts.resolved.url, "http://127.0.0.1:8888");
    EXPECT_EQ(opts.resolved.source, "manual");
}

// 场景:/proxy set 会清旧 fallback;/proxy reset 后回到 config,真正使用时重新
// probe config 代理,若仍不可达则再次 auto-fallback
TEST(ProxyResolverFallback, SessionResetReprobesConfigProxyOnUse) {
    StubProbe stub({TcpProbeReason::Refused, ""});
    ProxyResolver r;
    r.init(make_manual_cfg("http://127.0.0.1:8888"));
    r.probe_and_maybe_fallback();
    ASSERT_TRUE(r.is_fallback_active());

    r.set_session_override_url("http://otherproxy.local:3128");
    r.reset_session_override();
    auto eff = r.effective("https://api.example.com");
    EXPECT_EQ(eff.source, "manual");
    EXPECT_EQ(eff.url, "http://127.0.0.1:8888");

    auto opts = r.options_for("https://api.example.com");
    EXPECT_EQ(opts.resolved.source, "auto-fallback");
    EXPECT_FALSE(opts.proxies.has("http"));
    EXPECT_FALSE(opts.proxies.has("https"));
}

// 场景:proxy_mode=off 时不做 probe(没什么可探的)
TEST(ProxyResolverFallback, ProxyModeOffSkipsProbe) {
    StubProbe stub({TcpProbeReason::Refused, ""});
    NetworkConfig n;
    n.proxy_mode = "off";
    n.proxy_probe_enabled = true;
    ProxyResolver r;
    r.init(n);
    r.probe_and_maybe_fallback();

    EXPECT_FALSE(r.is_fallback_active());
}
