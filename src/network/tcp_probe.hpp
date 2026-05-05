#pragma once

// 跨平台同步 TCP connect 探测。供 ProxyResolver 在启动时判断"代理在不在
// 监听"用 — 不发任何字节,只看握手能否完成,失败按 errno/WSA error 翻译为
// 可读的 reason 枚举。
//
// openspec/changes/proxy-fallback-on-unreachable 设计要点:
//   - timeout 由调用方传入,平台实现负责把整体上限钉死(getaddrinfo + connect 总和)
//   - 注入式设计便于单测(set_tcp_probe_for_testing 替换默认实现)
//   - 默认实现走 getaddrinfo + 非阻塞 connect + poll/WSAPoll,无后台线程

#include <functional>
#include <string>

namespace acecode::network {

enum class TcpProbeReason {
    Ok,        // 三次握手成功
    Refused,   // ECONNREFUSED:对端 RST(代理进程没在监听端口)
    Timeout,   // 握手未在 timeout 内完成(网络不可达 / DNS 超时 / SYN 丢包)
    DnsFail,   // getaddrinfo 失败(host 无法解析)
    Other,     // 其它平台错误,detail 字段填 errno/WSA error 字符串
};

struct TcpProbeResult {
    TcpProbeReason reason = TcpProbeReason::Other;
    std::string detail;     // 人类可读的诊断字符串,空 = reason 已自描述
};

// 把 reason 转成短词("Ok" / "Refused" / "Timeout" / "DnsFail" / "Other")。
// 用于日志与 /proxy 输出。
const char* tcp_probe_reason_name(TcpProbeReason r);

// 默认实现 — 平台 cpp 文件里实例化(posix / win)。同步阻塞最长 timeout_ms。
TcpProbeResult tcp_probe(const std::string& host, int port, int timeout_ms);

// 测试注入接口:set 后所有 current_tcp_probe() 调用走桩函数;传 nullptr 恢复默认。
using TcpProbeFn = std::function<TcpProbeResult(const std::string&, int, int)>;
void set_tcp_probe_for_testing(TcpProbeFn fn);
TcpProbeFn current_tcp_probe();

} // namespace acecode::network
