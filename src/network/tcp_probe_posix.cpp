// POSIX 平台 tcp_probe 实现:getaddrinfo + 非阻塞 connect + poll。
//
// 整体超时上限钉死在 timeout_ms:
//   - getaddrinfo 在 POSIX 上是同步阻塞,无可移植的可中断版本,实践中本地
//     /etc/hosts + cached DNS 通常 < 50ms,corp DNS 慢的情况下也很少 > 500ms,
//     timeout_ms 默认 1500 给 connect 留 ~1000ms 是合理预算
//   - connect 走 O_NONBLOCK + poll,POLLOUT 命中 + getsockopt(SO_ERROR)==0 = Ok

#ifndef _WIN32

#include "tcp_probe.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace acecode::network {

namespace {

class FdGuard {
public:
    explicit FdGuard(int fd) : fd_(fd) {}
    ~FdGuard() { if (fd_ >= 0) ::close(fd_); }
    int get() const { return fd_; }
    int release() { int t = fd_; fd_ = -1; return t; }
private:
    int fd_;
};

TcpProbeResult try_one(const struct addrinfo* ai, int timeout_ms) {
    int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) {
        TcpProbeResult r; r.reason = TcpProbeReason::Other;
        r.detail = std::string("socket() errno=") + std::strerror(errno);
        return r;
    }
    FdGuard guard(fd);

    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        TcpProbeResult r; r.reason = TcpProbeReason::Other;
        r.detail = std::string("fcntl(O_NONBLOCK) errno=") + std::strerror(errno);
        return r;
    }

    int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (rc == 0) {
        // 立即成功(本机 listen socket)
        return {TcpProbeReason::Ok, ""};
    }
    if (errno != EINPROGRESS && errno != EWOULDBLOCK) {
        // 立即失败(常见为 ECONNREFUSED 在 Linux loopback 上)
        if (errno == ECONNREFUSED) return {TcpProbeReason::Refused, ""};
        TcpProbeResult r; r.reason = TcpProbeReason::Other;
        r.detail = std::string("connect() errno=") + std::strerror(errno);
        return r;
    }

    struct pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLOUT;
    int pr = ::poll(&pfd, 1, timeout_ms);
    if (pr == 0) return {TcpProbeReason::Timeout, ""};
    if (pr < 0) {
        TcpProbeResult r; r.reason = TcpProbeReason::Other;
        r.detail = std::string("poll() errno=") + std::strerror(errno);
        return r;
    }

    int sockerr = 0;
    socklen_t slen = sizeof(sockerr);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &sockerr, &slen) < 0) {
        TcpProbeResult r; r.reason = TcpProbeReason::Other;
        r.detail = std::string("getsockopt(SO_ERROR) errno=") + std::strerror(errno);
        return r;
    }
    if (sockerr == 0) return {TcpProbeReason::Ok, ""};
    if (sockerr == ECONNREFUSED) return {TcpProbeReason::Refused, ""};
    if (sockerr == ETIMEDOUT)    return {TcpProbeReason::Timeout, ""};
    TcpProbeResult r; r.reason = TcpProbeReason::Other;
    r.detail = std::string("connect SO_ERROR=") + std::strerror(sockerr);
    return r;
}

} // namespace

TcpProbeResult tcp_probe(const std::string& host, int port, int timeout_ms) {
    if (host.empty() || port <= 0 || port > 65535) {
        return {TcpProbeReason::Other, "invalid host/port"};
    }
    if (timeout_ms <= 0) timeout_ms = 1500;

    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    std::snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo* res = nullptr;
    int gai = ::getaddrinfo(host.c_str(), port_str, &hints, &res);
    if (gai != 0 || !res) {
        TcpProbeResult r; r.reason = TcpProbeReason::DnsFail;
        r.detail = ::gai_strerror(gai);
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

#endif // !_WIN32
