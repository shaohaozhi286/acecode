// 覆盖 src/network/proxy_resolver.{hpp,cpp} 的纯函数与 ProxyResolver 状态机。
// 这些是 Windows 下 Fiddler/Charles 抓包链路的入口,任何回归都直接让用户看不到
// 代理或泄露密码到日志,所以测试覆盖凭据脱敏、URL 规范化、NO_PROXY 匹配规则、
// 各 mode 的 effective() 输出 + session override 行为。Windows 的 winhttp 解析
// 走另一份测试文件(parse_winhttp_proxy_string),因为它是平台无关的纯函数。

#include <gtest/gtest.h>

#include "network/proxy_resolver.hpp"

#include <cstdlib>
#include <string>

#ifdef _WIN32
// Windows 下 setenv/unsetenv 不可用,用 _putenv_s 的简单包装。
static int test_setenv(const char* name, const char* val) {
    return ::_putenv_s(name, val);
}
static int test_unsetenv(const char* name) {
    return ::_putenv_s(name, "");
}
#else
static int test_setenv(const char* name, const char* val) {
    return ::setenv(name, val, 1);
}
static int test_unsetenv(const char* name) {
    return ::unsetenv(name);
}
#endif

using namespace acecode::network;

// 凭据脱敏:`http://user:pass@host` 必须把密码替换成 `***`,密码恰好 = 空字符串
// 时不破坏 URL 结构,user-only(无 `:pass`)不脱敏(没有需要保护的内容)。
TEST(RedactCredentials, ReplacesPasswordWithStars) {
    EXPECT_EQ(redact_credentials("http://alice:secret@proxy:8080"),
              "http://alice:***@proxy:8080");
    EXPECT_EQ(redact_credentials("https://user:pw@host:443/path?q=1"),
              "https://user:***@host:443/path?q=1");
}

TEST(RedactCredentials, LeavesUserOnlyAlone) {
    // 只有 user(无 `:`)不算密码,原样返回。
    EXPECT_EQ(redact_credentials("http://alice@proxy:8080"),
              "http://alice@proxy:8080");
}

TEST(RedactCredentials, LeavesPlainUrlAlone) {
    // 无 `@` 的 URL 与空字符串都直接返回,函数必须 idempotent。
    EXPECT_EQ(redact_credentials("http://proxy:8080"), "http://proxy:8080");
    EXPECT_EQ(redact_credentials(""), "");
}

// URL 规范化:无 scheme 时补 http://;含空格 / 不可解析时返回空字符串。
TEST(NormalizeProxyUrl, AddsHttpSchemeWhenMissing) {
    EXPECT_EQ(normalize_proxy_url("127.0.0.1:8888"), "http://127.0.0.1:8888");
    EXPECT_EQ(normalize_proxy_url("proxy.corp:3128"), "http://proxy.corp:3128");
}

TEST(NormalizeProxyUrl, KeepsExistingScheme) {
    EXPECT_EQ(normalize_proxy_url("https://corp:443"), "https://corp:443");
    EXPECT_EQ(normalize_proxy_url("socks5://localhost:1080"),
              "socks5://localhost:1080");
}

TEST(NormalizeProxyUrl, RejectsUnparseable) {
    // 含空格 / 空 host / 未知 scheme 都返回 ""。这些情况调用方会 fallback 直连。
    EXPECT_EQ(normalize_proxy_url(""), "");
    EXPECT_EQ(normalize_proxy_url("not a url"), "");
    EXPECT_EQ(normalize_proxy_url("ftp://x"), "");
    EXPECT_EQ(normalize_proxy_url("://nohost"), "");
}

TEST(NormalizeProxyUrl, TrimsLeadingTrailingWhitespace) {
    EXPECT_EQ(normalize_proxy_url("  127.0.0.1:8888  "), "http://127.0.0.1:8888");
}

// NO_PROXY CSV 解析与 host 匹配。规则覆盖:`.foo.com` 后缀、裸名后缀、`*` 通配。
TEST(NoProxy, ParseCsvIgnoresWhitespaceAndEmpties) {
    auto list = parse_no_proxy(" foo.com , .bar.com ,, baz ");
    ASSERT_EQ(list.size(), 3u);
    EXPECT_EQ(list[0], "foo.com");
    EXPECT_EQ(list[1], ".bar.com");
    EXPECT_EQ(list[2], "baz");
}

TEST(NoProxy, ExactHostMatches) {
    std::vector<std::string> list = {"api.openai.com"};
    EXPECT_TRUE(host_matches_no_proxy("api.openai.com", list));
    EXPECT_FALSE(host_matches_no_proxy("other.com", list));
}

TEST(NoProxy, DotPrefixIsSubdomainSuffix) {
    // `.foo.com` 匹配 bar.foo.com,libcurl 兼容上也匹配 foo.com 自身。
    std::vector<std::string> list = {".foo.com"};
    EXPECT_TRUE(host_matches_no_proxy("bar.foo.com", list));
    EXPECT_TRUE(host_matches_no_proxy("foo.com", list));
    EXPECT_FALSE(host_matches_no_proxy("notfoo.com", list));
}

TEST(NoProxy, BareNameMatchesSubdomains) {
    // 裸名 `foo.com` 也匹配 bar.foo.com(libcurl 行为)。
    std::vector<std::string> list = {"foo.com"};
    EXPECT_TRUE(host_matches_no_proxy("bar.foo.com", list));
    EXPECT_TRUE(host_matches_no_proxy("foo.com", list));
    EXPECT_FALSE(host_matches_no_proxy("xfoo.com", list));
}

TEST(NoProxy, WildcardMatchesEverything) {
    std::vector<std::string> list = {"*"};
    EXPECT_TRUE(host_matches_no_proxy("anything.test", list));
    EXPECT_TRUE(host_matches_no_proxy("openai.com", list));
}

TEST(NoProxy, CaseInsensitiveHostAndList) {
    std::vector<std::string> list = parse_no_proxy("API.OpenAI.COM");
    EXPECT_TRUE(host_matches_no_proxy("api.openai.com", list));
    EXPECT_TRUE(host_matches_no_proxy("API.OPENAI.COM", list));
}

// ProxyResolver 状态机:三种 mode + 三种 session override 的 effective() 行为。
// 这些是 /proxy 命令与启动横幅展示的来源,回归会直接让用户在调试时看错代理。

TEST(ProxyResolver, ModeOffAlwaysReturnsDirect) {
    acecode::NetworkConfig cfg;
    cfg.proxy_mode = "off";
    ProxyResolver r;
    r.init(cfg);
    auto res = r.effective("https://api.openai.com/v1/chat");
    EXPECT_TRUE(res.url.empty());
    EXPECT_EQ(res.source, "mode=off");
}

TEST(ProxyResolver, ModeManualReturnsConfiguredUrl) {
    acecode::NetworkConfig cfg;
    cfg.proxy_mode = "manual";
    cfg.proxy_url = "http://127.0.0.1:8888";
    ProxyResolver r;
    r.init(cfg);
    auto res = r.effective("https://api.openai.com/v1/chat");
    EXPECT_EQ(res.url, "http://127.0.0.1:8888");
    EXPECT_EQ(res.source, "manual");
}

TEST(ProxyResolver, ModeManualNormalizesSchemelessUrl) {
    acecode::NetworkConfig cfg;
    cfg.proxy_mode = "manual";
    cfg.proxy_url = "127.0.0.1:8888";
    ProxyResolver r;
    r.init(cfg);
    auto res = r.effective("https://api.example.com");
    EXPECT_EQ(res.url, "http://127.0.0.1:8888");
}

TEST(ProxyResolver, ModeManualUnparseableFallsBackToDirect) {
    acecode::NetworkConfig cfg;
    cfg.proxy_mode = "manual";
    cfg.proxy_url = "not a url with spaces";
    ProxyResolver r;
    r.init(cfg);
    auto res = r.effective("https://api.example.com");
    EXPECT_TRUE(res.url.empty());
    EXPECT_EQ(res.source, "none-after-error");
}

TEST(ProxyResolver, NoProxyBypassesProxiedHosts) {
    acecode::NetworkConfig cfg;
    cfg.proxy_mode = "manual";
    cfg.proxy_url = "http://127.0.0.1:8888";
    cfg.proxy_no_proxy = "api.github.com, .models.dev";
    ProxyResolver r;
    r.init(cfg);

    auto res_proxied = r.effective("https://api.openai.com/v1/chat");
    EXPECT_EQ(res_proxied.url, "http://127.0.0.1:8888");

    auto res_bypass = r.effective("https://api.github.com/login");
    EXPECT_TRUE(res_bypass.url.empty());
    EXPECT_EQ(res_bypass.source, "no_proxy");

    auto res_subdomain = r.effective("https://x.models.dev/api.json");
    EXPECT_TRUE(res_subdomain.url.empty());
    EXPECT_EQ(res_subdomain.source, "no_proxy");
}

TEST(ProxyResolver, SessionOverrideOffWinsOverConfigManual) {
    // 用户在 /proxy off 后,即使 config 里写了 manual url,也必须直连。
    acecode::NetworkConfig cfg;
    cfg.proxy_mode = "manual";
    cfg.proxy_url = "http://127.0.0.1:8888";
    ProxyResolver r;
    r.init(cfg);
    r.set_session_override_off();

    auto res = r.effective("https://api.openai.com");
    EXPECT_TRUE(res.url.empty());
    EXPECT_EQ(res.source, "session-override:off");
}

TEST(ProxyResolver, SessionOverrideUrlWinsOverConfigOff) {
    // 用户在 /proxy set <url> 后,即使 config 里 mode=off 也必须走该 URL。
    acecode::NetworkConfig cfg;
    cfg.proxy_mode = "off";
    ProxyResolver r;
    r.init(cfg);
    r.set_session_override_url("http://127.0.0.1:9999");

    auto res = r.effective("https://api.openai.com");
    EXPECT_EQ(res.url, "http://127.0.0.1:9999");
    EXPECT_EQ(res.source, "session-override");
}

TEST(ProxyResolver, ResetSessionOverrideRestoresConfig) {
    acecode::NetworkConfig cfg;
    cfg.proxy_mode = "manual";
    cfg.proxy_url = "http://primary:8888";
    ProxyResolver r;
    r.init(cfg);
    r.set_session_override_url("http://override:9999");
    EXPECT_EQ(r.effective("https://x.com").url, "http://override:9999");

    r.reset_session_override();
    EXPECT_EQ(r.effective("https://x.com").url, "http://primary:8888");
}

// auto 模式下的 env 探测。POSIX 上严格断言 source==env:HTTPS_PROXY;Windows
// 上跳过该断言 —— Windows env var 大小写不敏感,清理小写名会顺手删大写名,
// 而且开发机若启用了 IE 代理会优先走 winhttp。Windows 验收挪到手工 9.4。
#ifndef _WIN32
TEST(ProxyResolver, AutoModeReadsHttpsProxyEnv) {
    test_setenv("HTTPS_PROXY", "http://env-proxy:3128");
    // 清掉可能干扰的同义变量,确保来源唯一。
    test_unsetenv("HTTP_PROXY");
    test_unsetenv("ALL_PROXY");
    test_unsetenv("all_proxy");
    test_unsetenv("http_proxy");
    test_unsetenv("https_proxy");

    acecode::NetworkConfig cfg;
    cfg.proxy_mode = "auto";
    ProxyResolver r;
    r.init(cfg);
    auto res = r.effective("https://api.openai.com");

    EXPECT_EQ(res.url, "http://env-proxy:3128");
    EXPECT_EQ(res.source, "env:HTTPS_PROXY");

    test_unsetenv("HTTPS_PROXY");
}
#endif

// options_for() 把 ResolvedProxy 翻译成 cpr 可直接 splat 的结构。直连时
// proxies 必须为空,代理生效时 http+https 都要注入(让 libcurl 自己挑)。
// cpr::Proxies 没有 size() / empty(),只能通过 has(scheme) 探测。
TEST(ProxyResolver, OptionsForDirectGivesEmptyProxies) {
    acecode::NetworkConfig cfg;
    cfg.proxy_mode = "off";
    ProxyResolver r;
    r.init(cfg);
    auto opts = r.options_for("https://api.example.com");
    EXPECT_FALSE(opts.proxies.has("http"));
    EXPECT_FALSE(opts.proxies.has("https"));
    EXPECT_FALSE(opts.insecure);
    // resolved 字段保留来源,方便 /proxy 命令显示。
    EXPECT_EQ(opts.resolved.source, "mode=off");
}

TEST(ProxyResolver, OptionsForManualPopulatesBothSchemes) {
    acecode::NetworkConfig cfg;
    cfg.proxy_mode = "manual";
    cfg.proxy_url = "http://127.0.0.1:8888";
    ProxyResolver r;
    r.init(cfg);
    auto opts = r.options_for("https://api.example.com");
    EXPECT_TRUE(opts.proxies.has("http"));
    EXPECT_TRUE(opts.proxies.has("https"));
    EXPECT_FALSE(opts.insecure); // 默认不开 insecure_skip_verify
    EXPECT_EQ(opts.resolved.url, "http://127.0.0.1:8888");
}

TEST(ProxyResolver, InsecureSkipVerifyOnlyAppliesWhenProxyActive) {
    acecode::NetworkConfig cfg;
    cfg.proxy_mode = "manual";
    cfg.proxy_url = "http://127.0.0.1:8888";
    cfg.proxy_insecure_skip_verify = true;
    ProxyResolver r;
    r.init(cfg);

    auto opts_proxied = r.options_for("https://api.example.com");
    EXPECT_TRUE(opts_proxied.insecure);

    // 同样的 cfg,但 NO_PROXY 让该请求绕过代理 → insecure 不应生效。
    cfg.proxy_no_proxy = "api.example.com";
    r.init(cfg);
    auto opts_bypass = r.options_for("https://api.example.com");
    EXPECT_FALSE(opts_bypass.proxies.has("http"));
    EXPECT_FALSE(opts_bypass.proxies.has("https"));
    EXPECT_FALSE(opts_bypass.insecure);
}

// `parse_winhttp_proxy_string` 是 WinHTTP / 注册表 ProxyServer 字段格式解析,
// Windows 与 POSIX 都能 link,因为它本身没有 winhttp 依赖。
TEST(ParseWinHttpProxy, BareHostPortAppliesToAllSchemes) {
    EXPECT_EQ(parse_winhttp_proxy_string("127.0.0.1:8888", "https"),
              "http://127.0.0.1:8888");
    EXPECT_EQ(parse_winhttp_proxy_string("127.0.0.1:8888", "http"),
              "http://127.0.0.1:8888");
}

TEST(ParseWinHttpProxy, PerSchemeFormatPicksMatch) {
    EXPECT_EQ(parse_winhttp_proxy_string("http=p1:80;https=p2:443", "https"),
              "http://p2:443");
    EXPECT_EQ(parse_winhttp_proxy_string("http=p1:80;https=p2:443", "http"),
              "http://p1:80");
}

TEST(ParseWinHttpProxy, FallbackWhenSchemeMissing) {
    // 没有 https=... 时 fallback 到第一个 http/https 条目。
    EXPECT_EQ(parse_winhttp_proxy_string("http=p1:80", "https"),
              "http://p1:80");
}

TEST(ParseWinHttpProxy, EmptyAndWhitespaceReturnEmpty) {
    EXPECT_EQ(parse_winhttp_proxy_string("", "https"), "");
    EXPECT_EQ(parse_winhttp_proxy_string("   ", "https"), "");
}

TEST(ParseWinHttpProxy, IgnoresUnknownKeys) {
    // socks=... 当前实现忽略;ftp=... 一样不做隐式 fallback。
    EXPECT_EQ(parse_winhttp_proxy_string("socks=p:1080", "https"), "");
    EXPECT_EQ(parse_winhttp_proxy_string("ftp=p:21;https=h:443", "https"),
              "http://h:443");
}
