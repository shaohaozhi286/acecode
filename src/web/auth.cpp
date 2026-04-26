#include "auth.hpp"

namespace acecode::web {

bool is_loopback_address(std::string_view ip) {
    if (ip.empty()) return false;
    // IPv4
    if (ip == "127.0.0.1") return true;
    // 任意 127.0.0.0/8 都视为 loopback(规范化前缀检查)
    if (ip.size() > 4 && ip.substr(0, 4) == "127.") return true;
    // IPv6 标准 loopback
    if (ip == "::1") return true;
    // IPv4-mapped IPv6 loopback (::ffff:127.0.0.1)
    if (ip.size() >= 7 && ip.substr(0, 7) == "::ffff:") {
        return is_loopback_address(ip.substr(7));
    }
    return false;
}

std::string preflight_bind_check(const std::string& bind,
                                   const std::string& server_token,
                                   bool dangerous) {
    bool loopback = is_loopback_address(bind);
    if (!loopback && server_token.empty()) {
        return "non-loopback bind requires token; please configure web.require_token "
               "or restrict web.bind to 127.0.0.1";
    }
    if (dangerous && !loopback) {
        return "dangerous mode is loopback-only (web.bind=" + bind + ")";
    }
    return {};
}

AuthResult check_request_auth(std::string_view client_ip,
                                std::string_view server_token,
                                std::string_view header_token,
                                std::string_view query_token) {
    if (is_loopback_address(client_ip)) return AuthResult::Allowed;
    // 非 loopback: 强制 token
    if (header_token.empty() && query_token.empty()) return AuthResult::NoToken;
    if (!server_token.empty() &&
        (header_token == server_token || query_token == server_token)) {
        return AuthResult::Allowed;
    }
    return AuthResult::BadToken;
}

} // namespace acecode::web
