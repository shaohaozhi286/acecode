#pragma once

// HTTP Origin 头解析(纯字符串处理,无 Crow / 无 daemon 状态)。
// 给 src/web/server.cpp 的同源 / loopback 判定用 — 拆出来便于单测覆盖。

#include <string>

namespace acecode::web {

struct OriginParts {
    std::string scheme;
    std::string host;
    std::string port;
};

// 拆 "host[:port]" 或 "[ipv6]:port"。空 port 留空 — 调用方按 scheme 决定默认。
// host / port 都被 lowercase。
OriginParts split_host_port(std::string hostport);

// 解析完整 origin "scheme://host[:port][/...]"。无 "://" 返回空 OriginParts。
// scheme + host 都 lowercase。port 缺省时按 scheme 填:http→80,https→443。
OriginParts parse_origin(const std::string& origin);

} // namespace acecode::web
