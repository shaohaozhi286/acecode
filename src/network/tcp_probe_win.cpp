// Windows 平台 tcp_probe 实现:WSAStartup + getaddrinfo + 非阻塞 connect + WSAPoll。

#ifdef _WIN32

#include "tcp_probe.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cstdio>
#include <mutex>

namespace acecode::network {

namespace {

// 进程级懒初始化 WSA。多次调用 WSAStartup 是 OK 的(refcount),但我们只想付一次启动代价。
void ensure_wsa_startup() {
    static std::once_flag once;
    std::call_once(once, []() {
        WSADATA d;
        ::WSAStartup(MAKEWORD(2, 2), &d);
    });
}

class SockGuard {
public:
    explicit SockGuard(SOCKET s) : s_(s) {}
    ~SockGuard() { if (s_ != INVALID_SOCKET) ::closesocket(s_); }
    SOCKET get() const { return s_; }
private:
    SOCKET s_;
};

TcpProbeResult try_one(const struct addrinfo* ai, int timeout_ms) {
    SOCKET s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (s == INVALID_SOCKET) {
        TcpProbeResult r; r.reason = TcpProbeReason::Other;
        r.detail = "socket() WSAErr=" + std::to_string(::WSAGetLastError());
        return r;
    }
    SockGuard guard(s);

    u_long mode = 1;
    if (::ioctlsocket(s, FIONBIO, &mode) != 0) {
        TcpProbeResult r; r.reason = TcpProbeReason::Other;
        r.detail = "ioctlsocket(FIONBIO) WSAErr=" + std::to_string(::WSAGetLastError());
        return r;
    }

    int rc = ::connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
    if (rc == 0) return {TcpProbeReason::Ok, ""};

    int err = ::WSAGetLastError();
    if (err != WSAEWOULDBLOCK) {
        if (err == WSAECONNREFUSED) return {TcpProbeReason::Refused, ""};
        TcpProbeResult r; r.reason = TcpProbeReason::Other;
        r.detail = "connect() WSAErr=" + std::to_string(err);
        return r;
    }

    WSAPOLLFD pfd{};
    pfd.fd = s;
    pfd.events = POLLWRNORM;
    int pr = ::WSAPoll(&pfd, 1, timeout_ms);
    if (pr == 0) return {TcpProbeReason::Timeout, ""};
    if (pr < 0) {
        TcpProbeResult r; r.reason = TcpProbeReason::Other;
        r.detail = "WSAPoll() WSAErr=" + std::to_string(::WSAGetLastError());
        return r;
    }

    int sockerr = 0;
    int slen = sizeof(sockerr);
    if (::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&sockerr), &slen) != 0) {
        TcpProbeResult r; r.reason = TcpProbeReason::Other;
        r.detail = "getsockopt(SO_ERROR) WSAErr=" + std::to_string(::WSAGetLastError());
        return r;
    }
    if (sockerr == 0) return {TcpProbeReason::Ok, ""};
    if (sockerr == WSAECONNREFUSED) return {TcpProbeReason::Refused, ""};
    if (sockerr == WSAETIMEDOUT)    return {TcpProbeReason::Timeout, ""};
    TcpProbeResult r; r.reason = TcpProbeReason::Other;
    r.detail = "connect SO_ERROR=" + std::to_string(sockerr);
    return r;
}

} // namespace

TcpProbeResult tcp_probe(const std::string& host, int port, int timeout_ms) {
    if (host.empty() || port <= 0 || port > 65535) {
        return {TcpProbeReason::Other, "invalid host/port"};
    }
    if (timeout_ms <= 0) timeout_ms = 1500;

    ensure_wsa_startup();

    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    std::snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo* res = nullptr;
    int gai = ::getaddrinfo(host.c_str(), port_str, &hints, &res);
    if (gai != 0 || !res) {
        TcpProbeResult r; r.reason = TcpProbeReason::DnsFail;
        r.detail = "getaddrinfo() WSAErr=" + std::to_string(gai);
        return r;
    }

    TcpProbeResult last{TcpProbeReason::Other, "no addresses returned"};
    for (auto* ai = res; ai != nullptr; ai = ai->ai_next) {
        last = try_one(ai, timeout_ms);
        if (last.reason == TcpProbeReason::Ok) break;
    }
    ::freeaddrinfo(res);
    return last;
}

} // namespace acecode::network

#endif // _WIN32
