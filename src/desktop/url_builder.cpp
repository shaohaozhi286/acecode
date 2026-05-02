#include "url_builder.hpp"

#include <sstream>
#include <iomanip>

namespace acecode::desktop {

std::string percent_encode(const std::string& raw) {
    // RFC 3986 unreserved: ALPHA / DIGIT / "-" / "." / "_" / "~"
    auto is_unreserved = [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') ||
               (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') ||
               c == '-' || c == '.' || c == '_' || c == '~';
    };

    std::ostringstream out;
    out.fill('0');
    out << std::hex << std::uppercase;
    for (unsigned char c : raw) {
        if (is_unreserved(c)) {
            out << static_cast<char>(c);
        } else {
            // 强制两位十六进制,空格/+/%/= 等都编成 %XX
            out << '%' << std::setw(2) << static_cast<int>(c);
        }
    }
    return out.str();
}

std::string build_loopback_url(int port, const std::string& token) {
    std::ostringstream u;
    u << "http://127.0.0.1:" << port << "/";
    if (!token.empty()) {
        u << "?token=" << percent_encode(token);
    }
    return u.str();
}

} // namespace acecode::desktop
