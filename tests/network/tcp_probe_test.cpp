// 覆盖 src/network/tcp_probe.{cpp,_posix.cpp,_win.cpp} 的真实平台行为。
//
// 三个 reason 用本地 socket 触发,不打公网:
//   - Ok:     127.0.0.1 上自己 listen,probe 命中 SYN-ACK
//   - Refused: 127.0.0.1 上 listen 后立即 close,内核回 RST
//   - Timeout: TEST-NET-1 (192.0.2.0/24) 是 IANA 保留的不可路由段,SYN 黑洞,
//              probe 必然 timeout(200ms 上限,测试快)
//
// 单元测试 pattern 参考 tests/web/skills_handler_test.cpp:gtest fixture +
// RAII socket guard;Windows / POSIX 用条件编译切换底层 API。

#include <gtest/gtest.h>

#include "network/tcp_probe.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
inline void close_sock(socket_t s) { ::closesocket(s); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
inline void close_sock(socket_t s) { ::close(s); }
#endif

namespace {

class SockRAII {
public:
    explicit SockRAII(socket_t s = kInvalidSocket) : s_(s) {}
    ~SockRAII() { if (s_ != kInvalidSocket) close_sock(s_); }
    SockRAII(const SockRAII&) = delete;
    SockRAII& operator=(const SockRAII&) = delete;
    socket_t get() const { return s_; }
    socket_t release() { socket_t t = s_; s_ = kInvalidSocket; return t; }
    void reset(socket_t s = kInvalidSocket) {
        if (s_ != kInvalidSocket) close_sock(s_);
        s_ = s;
    }
private:
    socket_t s_;
};

// 在 127.0.0.1 上 bind+listen,返回 (socket, port)。失败时 socket = kInvalidSocket。
std::pair<socket_t, int> open_listener_on_loopback() {
#ifdef _WIN32
    static struct WsaInit { WsaInit() { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); } } _;
#endif
    socket_t s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == kInvalidSocket) return {kInvalidSocket, 0};

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; // 让内核分配空闲端口

    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close_sock(s);
        return {kInvalidSocket, 0};
    }
    if (::listen(s, 4) != 0) {
        close_sock(s);
        return {kInvalidSocket, 0};
    }

    sockaddr_in bound{};
#ifdef _WIN32
    int slen = sizeof(bound);
#else
    socklen_t slen = sizeof(bound);
#endif
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&bound), &slen) != 0) {
        close_sock(s);
        return {kInvalidSocket, 0};
    }
    return {s, ntohs(bound.sin_port)};
}

} // namespace

using acecode::network::tcp_probe;
using acecode::network::TcpProbeReason;

// 场景:Ok — 本地 listen socket 端口 connect 应当握手成功
TEST(TcpProbe, OkOnLocalListenSocket) {
    auto [s, port] = open_listener_on_loopback();
    ASSERT_NE(s, kInvalidSocket) << "failed to open listener";
    SockRAII guard(s);

    auto r = tcp_probe("127.0.0.1", port, 1000);
    EXPECT_EQ(r.reason, TcpProbeReason::Ok) << "got: " << r.detail;
}

// 场景:Refused — listener 已经关闭,内核回 RST,probe 应当返回 Refused
TEST(TcpProbe, RefusedAfterListenerClosed) {
    auto [s, port] = open_listener_on_loopback();
    ASSERT_NE(s, kInvalidSocket);
    close_sock(s); // 立即关掉,端口暂时还在 TIME_WAIT 但 connect 会被拒

    auto r = tcp_probe("127.0.0.1", port, 1000);
    // POSIX/Windows 在 loopback 都会立即给 RST → Refused;某些 NAT/防火墙
    // 边缘可能丢包后 timeout,这里接受任一(不应 Ok / DnsFail)
    EXPECT_TRUE(r.reason == TcpProbeReason::Refused ||
                r.reason == TcpProbeReason::Timeout)
        << "expected Refused or Timeout, got: " << r.detail;
    EXPECT_NE(r.reason, TcpProbeReason::Ok);
}

// 场景:Timeout — 192.0.2.1 是 RFC5737 TEST-NET-1 保留地址,SYN 包会被路由层
// 黑洞或丢弃,connect 必然在 timeout 内不返回。设短 timeout 让用例跑得快。
TEST(TcpProbe, TimeoutOnUnreachableHost) {
    auto r = tcp_probe("192.0.2.1", 1, 250);
    // 大多数环境 SYN 直接被 drop → Timeout;某些防火墙可能立即回 ICMP
    // unreachable → 表现为 Other(平台不同);不应 Ok / Refused / DnsFail。
    EXPECT_NE(r.reason, TcpProbeReason::Ok);
    EXPECT_NE(r.reason, TcpProbeReason::Refused);
    EXPECT_NE(r.reason, TcpProbeReason::DnsFail);
}

// 场景:DnsFail — 用一个肯定不存在的 TLD,getaddrinfo 失败。
// 用 .invalid TLD(RFC2606 保留),保证无 DNS 命中。
TEST(TcpProbe, DnsFailOnNonexistentHost) {
    auto r = tcp_probe("nonexistent-host.invalid", 80, 1000);
    EXPECT_EQ(r.reason, TcpProbeReason::DnsFail) << "got: " << r.detail;
}

// 场景:非法参数防御 — port 越界 / 空 host 应当 Other
TEST(TcpProbe, RejectsInvalidArgs) {
    auto r1 = tcp_probe("127.0.0.1", 0, 1000);
    EXPECT_EQ(r1.reason, TcpProbeReason::Other);
    auto r2 = tcp_probe("127.0.0.1", 70000, 1000);
    EXPECT_EQ(r2.reason, TcpProbeReason::Other);
    auto r3 = tcp_probe("", 80, 1000);
    EXPECT_EQ(r3.reason, TcpProbeReason::Other);
}

// 场景:reason_name 字符串 — 防止有人改 enum 不同步 switch
TEST(TcpProbeReasonName, AllReasonsHaveDistinctName) {
    using acecode::network::tcp_probe_reason_name;
    EXPECT_STREQ(tcp_probe_reason_name(TcpProbeReason::Ok),      "Ok");
    EXPECT_STREQ(tcp_probe_reason_name(TcpProbeReason::Refused), "Refused");
    EXPECT_STREQ(tcp_probe_reason_name(TcpProbeReason::Timeout), "Timeout");
    EXPECT_STREQ(tcp_probe_reason_name(TcpProbeReason::DnsFail), "DnsFail");
    EXPECT_STREQ(tcp_probe_reason_name(TcpProbeReason::Other),   "Other");
}
