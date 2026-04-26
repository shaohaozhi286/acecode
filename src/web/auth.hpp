#pragma once

// Auth helper: 鉴权策略集中处理(openspec add-web-daemon Section 11)。
//
// 设计原则:
//   - loopback (127.0.0.1 / ::1) 不强制 token,保持本地用户体验
//   - 非 loopback 必须带正确 token (header X-ACECode-Token 或 query ?token=)
//   - 启动期硬检查: 非 loopback 缺 token / -dangerous 与远程绑定互斥 → 拒启
//
// 接口纯函数,不依赖 Crow,方便单元测试。Crow 路由层(server.cpp)在每个
// handler / WebSocket onaccept 里调 check_request_auth。

#include <string>
#include <string_view>

namespace acecode::web {

enum class AuthResult {
    Allowed,                // 通过(loopback 或 token 正确)
    NoToken,                // 非 loopback 但请求未带 token → 401
    BadToken,               // 非 loopback 且 token 错误 → 401
};

// 判断 ip 是否为 loopback。空字符串视为非 loopback(保守拒绝)。
// IPv4 形如 "127.0.0.1",IPv6 形如 "::1" 或 "::ffff:127.0.0.1"。
bool is_loopback_address(std::string_view ip);

// 启动期检查:web_bind 配置 + dangerous 标记下能否启动。
// 返回空字符串 = 允许;非空 = 拒启理由(由调用方打印 + 非零退出)。
//
// 规则:
//   - bind != loopback && server_token.empty()              → "non-loopback bind requires token"
//   - dangerous && bind != loopback                          → "dangerous mode is loopback-only"
//   - 其余通过
std::string preflight_bind_check(const std::string& bind,
                                   const std::string& server_token,
                                   bool dangerous);

// 请求鉴权: client_ip 是 Crow 的 req.remote_ip_address;header_token 是
// X-ACECode-Token 头的值(空表示没带);query_token 是 URL 参数 ?token= 的值
// (WebSocket 用)。server_token 是 daemon 启动时生成的真 token。
//
// 任意一边匹配即通过(便于浏览器 fetch 用 header,WS upgrade 必须用 query)。
AuthResult check_request_auth(std::string_view client_ip,
                                std::string_view server_token,
                                std::string_view header_token,
                                std::string_view query_token);

} // namespace acecode::web
