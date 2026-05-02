#pragma once

// desktop shell 启动 daemon 子进程后,用拿到的 port + token 拼出
// http://127.0.0.1:<port>/?token=<encoded> 给 WebView 加载。
//
// 设计原因: token 里允许出现 + / = 等 base64 字符或 % 等保留字符,直接拼到
// query string 里会被浏览器误解析,需要 percent-encode。loopback host 固定
// 127.0.0.1 — 不走 localhost 是为了避开个别 Windows 配置下 localhost 解析
// 走 IPv6 的回归到不同 socket 的问题。
//
// 纯函数,无系统依赖,链入 acecode_testable 做单元测试。

#include <string>

namespace acecode::desktop {

// 把 token(任意 ASCII 字节)按 RFC 3986 unreserved 集合做 percent-encoding。
// 暴露为公开符号便于直接测试。
std::string percent_encode(const std::string& raw);

// 给定 port + token 拼出 WebView 用的初始 URL。port 不做范围校验(由调用方保证)。
std::string build_loopback_url(int port, const std::string& token);

} // namespace acecode::desktop
