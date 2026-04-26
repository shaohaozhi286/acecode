// 覆盖 src/web/auth.{hpp,cpp}。这是 daemon HTTP/WS 鉴权(spec Section 11)
// 的核心策略层 — 一旦回归:
//   - 远程(非 loopback)请求被错放行 → 安全洞
//   - 启动期硬检查失误 → daemon 跑起来才发现 bind/-dangerous 配置非法,
//     此时端口已开,危险窗口
//   - loopback IP 识别错误 → 本机正常用户被强制要 token,体验崩
//
// 这些测试是纯函数级别,不起 Crow,不动磁盘,毫秒级跑完。

#include <gtest/gtest.h>

#include "web/auth.hpp"

using namespace acecode::web;

// ============================================================
// is_loopback_address: IPv4/IPv6/v4-mapped-v6/边角
// ============================================================

// 场景: 标准 IPv4 loopback。HTTP handler 拿到 req.remote_ip_address ==
// "127.0.0.1" 时必须放行。
TEST(IsLoopback, StandardIPv4) {
    EXPECT_TRUE(is_loopback_address("127.0.0.1"));
}

// 场景: 整个 127.0.0.0/8 都是 loopback(POSIX 标准),不止 .1。
TEST(IsLoopback, EntireIPv4LoopbackBlock) {
    EXPECT_TRUE(is_loopback_address("127.1.2.3"));
    EXPECT_TRUE(is_loopback_address("127.255.255.254"));
    EXPECT_TRUE(is_loopback_address("127.0.0.0"));
}

// 场景: 标准 IPv6 loopback。Crow 在 IPv6 socket 上 accept 时给的就是 "::1"。
TEST(IsLoopback, StandardIPv6) {
    EXPECT_TRUE(is_loopback_address("::1"));
}

// 场景: IPv4-mapped IPv6 loopback。Linux kernel 在 dual-stack socket 上给
// 的是 "::ffff:127.0.0.1" 而不是裸 "127.0.0.1" — 必须识别。
TEST(IsLoopback, IPv4MappedIPv6Loopback) {
    EXPECT_TRUE(is_loopback_address("::ffff:127.0.0.1"));
    EXPECT_TRUE(is_loopback_address("::ffff:127.5.6.7"));
}

// 场景: 公网 IP / LAN IP / 空字符串 — 都不算 loopback。空字符串特别危险
// (假如某个未来代码路径忘填 remote_ip,我们应保守拒绝)。
TEST(IsLoopback, NonLoopbackAddresses) {
    EXPECT_FALSE(is_loopback_address("8.8.8.8"));
    EXPECT_FALSE(is_loopback_address("192.168.1.10"));
    EXPECT_FALSE(is_loopback_address("10.0.0.1"));
    EXPECT_FALSE(is_loopback_address("2001:db8::1"));
    EXPECT_FALSE(is_loopback_address(""));
}

// 场景: "127" 前缀但不是合法 loopback 段 — 比如 "127abc" 或 "1270.0.0.1"。
// 当前实现只查 "127."(带点),所以 "127a..." 不应误匹配。
TEST(IsLoopback, AlmostLoopbackButNot) {
    EXPECT_FALSE(is_loopback_address("128.0.0.1"));
    EXPECT_FALSE(is_loopback_address("1.2.3.4"));
    // 边角: 没有点,只是字符串里有 "127"
    EXPECT_FALSE(is_loopback_address("12.7.0.1"));
}

// ============================================================
// preflight_bind_check: 启动期硬检查
// ============================================================

// 场景: loopback + 任何 token + 非 dangerous → 通过(最常见配置)。
TEST(Preflight, LoopbackPasses) {
    EXPECT_EQ(preflight_bind_check("127.0.0.1", "token123", false), "");
    EXPECT_EQ(preflight_bind_check("::1", "token123", false), "");
    // loopback + 没 token 也通过 — loopback 本来就不强制 token
    EXPECT_EQ(preflight_bind_check("127.0.0.1", "", false), "");
}

// 场景: 非 loopback 必须有 token。否则启动期就要拒,不能让端口开起来再发现。
TEST(Preflight, NonLoopbackWithoutTokenRejected) {
    auto err = preflight_bind_check("0.0.0.0", "", false);
    EXPECT_FALSE(err.empty()) << "0.0.0.0 + 无 token 应被拒";
    err = preflight_bind_check("192.168.1.5", "", false);
    EXPECT_FALSE(err.empty());
}

// 场景: 非 loopback + 有 token → 通过。
TEST(Preflight, NonLoopbackWithTokenPasses) {
    EXPECT_EQ(preflight_bind_check("0.0.0.0", "real-token", false), "");
}

// 场景: dangerous + 远程绑定互斥(spec 11.3 第二条硬检查)。
// 哪怕带了 token 也必须拒。
TEST(Preflight, DangerousNonLoopbackRejected) {
    auto err = preflight_bind_check("0.0.0.0", "token", true);
    EXPECT_FALSE(err.empty()) << "dangerous + 0.0.0.0 必须拒";
    EXPECT_NE(err.find("loopback-only"), std::string::npos)
        << "错误信息应明确提示 loopback-only";
}

// 场景: dangerous + loopback → 通过(本机调试场景)。
TEST(Preflight, DangerousLoopbackPasses) {
    EXPECT_EQ(preflight_bind_check("127.0.0.1", "tk", true), "");
    EXPECT_EQ(preflight_bind_check("::1", "tk", true), "");
}

// ============================================================
// check_request_auth: 每次请求的鉴权决策
// ============================================================

// 场景: loopback 客户端不强制 token(本机 = 信任域,spec 11.4 / decision 7)。
TEST(CheckAuth, LoopbackAlwaysAllowed) {
    EXPECT_EQ(check_request_auth("127.0.0.1", "server-tk", "", ""),
              AuthResult::Allowed);
    EXPECT_EQ(check_request_auth("::1", "server-tk", "wrong-tk", ""),
              AuthResult::Allowed) << "loopback 即使 token 错也放行";
    EXPECT_EQ(check_request_auth("::ffff:127.0.0.1", "", "", ""),
              AuthResult::Allowed);
}

// 场景: 非 loopback + 完全没带 token → NoToken (HTTP 401 reason="no token")。
TEST(CheckAuth, NonLoopbackNoTokenIsNoToken) {
    EXPECT_EQ(check_request_auth("8.8.8.8", "server-tk", "", ""),
              AuthResult::NoToken);
}

// 场景: 非 loopback + token 错 → BadToken (HTTP 401 reason="bad token")。
// 区分 NoToken / BadToken 是为了日志可定位 — 客户端方真没传 vs 传错。
TEST(CheckAuth, NonLoopbackBadTokenIsBadToken) {
    EXPECT_EQ(check_request_auth("8.8.8.8", "server-tk", "wrong", ""),
              AuthResult::BadToken);
    EXPECT_EQ(check_request_auth("8.8.8.8", "server-tk", "", "wrong"),
              AuthResult::BadToken);
}

// 场景: 非 loopback + header_token 正确 → 通过(浏览器 fetch 主路径)。
TEST(CheckAuth, NonLoopbackHeaderTokenAllows) {
    EXPECT_EQ(check_request_auth("8.8.8.8", "server-tk", "server-tk", ""),
              AuthResult::Allowed);
}

// 场景: 非 loopback + query token 正确 → 通过(WebSocket upgrade 主路径,
// 浏览器没法在 WS handshake 上加 header)。
TEST(CheckAuth, NonLoopbackQueryTokenAllows) {
    EXPECT_EQ(check_request_auth("8.8.8.8", "server-tk", "", "server-tk"),
              AuthResult::Allowed);
}

// 场景: server_token 为空 = 服务端没生成 token → 任何非 loopback 请求拒。
// 防止 bug:启动时 token 生成失败但 daemon 还是跑起来,远程访问应该被全拒。
TEST(CheckAuth, EmptyServerTokenRejectsNonLoopback) {
    EXPECT_EQ(check_request_auth("8.8.8.8", "", "", ""),
              AuthResult::NoToken);
    // 哪怕客户端"匹配上了空字符串" 也不放行 — server_token.empty() 时硬拒
    EXPECT_EQ(check_request_auth("8.8.8.8", "", "", ""),
              AuthResult::NoToken);
}

// 场景: header 与 query 都给了,但只有一个对 → 仍然通过(任意一边匹配即可)。
// 这是设计选择: 简化客户端实现,header 优先但 query 兜底。
TEST(CheckAuth, EitherTokenSourceMatchesIsEnough) {
    EXPECT_EQ(check_request_auth("8.8.8.8", "tk", "tk", "wrong"),
              AuthResult::Allowed);
    EXPECT_EQ(check_request_auth("8.8.8.8", "tk", "wrong", "tk"),
              AuthResult::Allowed);
}
