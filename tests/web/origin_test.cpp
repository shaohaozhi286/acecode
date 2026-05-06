// 覆盖 src/web/origin.{hpp,cpp}。这是 daemon 的 HTTP Origin 头解析层 ——
// 同源 / loopback 判定的输入,任何回归都会让 server.cpp 的 CSRF / cross-origin
// 防护误判,直接放行恶意 origin 或反过来打断本机正常请求。
//
// 这些测试是纯函数级别 —— 不动 Crow,不动磁盘,毫秒级跑完。
// 与现有 tests/web/auth_test.cpp 的风格一致。

#include <gtest/gtest.h>

#include "web/origin.hpp"

using namespace acecode::web;

// ============================================================
// parse_origin: scheme + host + port 拆分
// ============================================================

// 场景: 标准 http origin,显式端口。最常见的 daemon 自身请求形态。
TEST(ParseOrigin, HttpExplicitPort) {
    auto o = parse_origin("http://localhost:28080");
    EXPECT_EQ(o.scheme, "http");
    EXPECT_EQ(o.host, "localhost");
    EXPECT_EQ(o.port, "28080");
}

// 场景: https origin 显式端口。处理路径与 http 完全相同,只是默认端口不同。
TEST(ParseOrigin, HttpsExplicitPort) {
    auto o = parse_origin("https://example.com:8443");
    EXPECT_EQ(o.scheme, "https");
    EXPECT_EQ(o.host, "example.com");
    EXPECT_EQ(o.port, "8443");
}

// 场景: 缺省端口由 scheme 推断 —— http→80,https→443。
// is_same_request_origin 需要 port 永远非空,这里是兜底。
TEST(ParseOrigin, DefaultPortForHttp) {
    auto o = parse_origin("http://example.com");
    EXPECT_EQ(o.scheme, "http");
    EXPECT_EQ(o.host, "example.com");
    EXPECT_EQ(o.port, "80");
}

TEST(ParseOrigin, DefaultPortForHttps) {
    auto o = parse_origin("https://example.com");
    EXPECT_EQ(o.scheme, "https");
    EXPECT_EQ(o.host, "example.com");
    EXPECT_EQ(o.port, "443");
}

// 场景: scheme 与 host 都被 lowercase。Origin header 大小写不敏感,但下游
// 比较是逐字符的 —— 不归一化会让 "HTTP://Foo" 与 "http://foo" 被判为不同源。
TEST(ParseOrigin, LowercasesSchemeAndHost) {
    auto o = parse_origin("HTTP://EXAMPLE.COM:8080");
    EXPECT_EQ(o.scheme, "http");
    EXPECT_EQ(o.host, "example.com");
    EXPECT_EQ(o.port, "8080");
}

// 场景: IPv6 字面量包在方括号里。crow / 浏览器都按 RFC 3986 发,
// "http://[::1]:28080" 必须正确拆出 host="::1" port="28080"。
TEST(ParseOrigin, IPv6BracketedHost) {
    auto o = parse_origin("http://[::1]:28080");
    EXPECT_EQ(o.scheme, "http");
    EXPECT_EQ(o.host, "::1");
    EXPECT_EQ(o.port, "28080");
}

// 场景: IPv6 字面量缺省端口 —— 仍需按 scheme 兜底端口。
TEST(ParseOrigin, IPv6BracketedDefaultPort) {
    auto o = parse_origin("https://[::1]");
    EXPECT_EQ(o.scheme, "https");
    EXPECT_EQ(o.host, "::1");
    EXPECT_EQ(o.port, "443");
}

// 场景: origin 带尾部路径 —— 浏览器一般只发 scheme://host[:port],但有些
// 客户端会带 "/"。path 部分必须被忽略,只保留 authority。
TEST(ParseOrigin, TrailingSlashPathIgnored) {
    auto o = parse_origin("http://example.com:1234/some/path");
    EXPECT_EQ(o.scheme, "http");
    EXPECT_EQ(o.host, "example.com");
    EXPECT_EQ(o.port, "1234");
}

// 场景: 缺 "://" 必须返回空 OriginParts。Origin 头没这个分隔符就是不合法,
// 不应该当成 host 名硬塞 — 否则 is_same_request_origin 会拿空 scheme 比对。
TEST(ParseOrigin, MissingSchemeReturnsEmpty) {
    auto o = parse_origin("example.com:8080");
    EXPECT_TRUE(o.scheme.empty());
    EXPECT_TRUE(o.host.empty());
    EXPECT_TRUE(o.port.empty());
}

// 场景: 完全空字符串 —— 与缺 "://" 同样路径,返回空。
TEST(ParseOrigin, EmptyInputReturnsEmpty) {
    auto o = parse_origin("");
    EXPECT_TRUE(o.scheme.empty());
    EXPECT_TRUE(o.host.empty());
    EXPECT_TRUE(o.port.empty());
}

// ============================================================
// split_host_port: Host 头(无 scheme)的纯 authority 拆分
// ============================================================

// 场景: 标准 "host:port"。Host header 在 HTTP/1.1 永远带 port(虽然规范上可省)。
TEST(SplitHostPort, StandardHostPort) {
    auto hp = split_host_port("example.com:1234");
    EXPECT_EQ(hp.host, "example.com");
    EXPECT_EQ(hp.port, "1234");
}

// 场景: 只 host 不带 port —— port 留空,调用方按 scheme 决定默认。
TEST(SplitHostPort, HostOnlyEmptyPort) {
    auto hp = split_host_port("example.com");
    EXPECT_EQ(hp.host, "example.com");
    EXPECT_TRUE(hp.port.empty());
}

// 场景: IPv6 方括号 + 端口。判定方括号要早于"找最后一个冒号"逻辑,
// 否则 "[::1]:28080" 的最后一个冒号会落在 "1]" 后面,host 就拆错了。
TEST(SplitHostPort, IPv6BracketedWithPort) {
    auto hp = split_host_port("[::1]:28080");
    EXPECT_EQ(hp.host, "::1");
    EXPECT_EQ(hp.port, "28080");
}

// 场景: 裸 IPv6 含多个冒号且没方括号。代码用 "rfind == find" 启发式判定
// 是否只有一个冒号,多冒号且没方括号视为 IPv6 host 整段保留 — 不拆 port。
// 这条测覆盖该启发式分支。
TEST(SplitHostPort, BareIPv6MultipleColonsKeepWhole) {
    auto hp = split_host_port("fe80::1");
    EXPECT_EQ(hp.host, "fe80::1");
    EXPECT_TRUE(hp.port.empty());
}

// 场景: host 与 port 都被 lowercase。host 大小写不敏感,与 parse_origin 行为一致。
TEST(SplitHostPort, LowercasesHost) {
    auto hp = split_host_port("EXAMPLE.com:80");
    EXPECT_EQ(hp.host, "example.com");
    EXPECT_EQ(hp.port, "80");
}
