#include "origin.hpp"

#include <algorithm>
#include <cctype>

namespace acecode::web {

namespace {

std::string ascii_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

} // namespace

OriginParts split_host_port(std::string hostport) {
    OriginParts out;
    hostport = ascii_lower(std::move(hostport));
    if (!hostport.empty() && hostport.front() == '[') {
        auto end = hostport.find(']');
        if (end != std::string::npos) {
            out.host = hostport.substr(1, end - 1);
            if (end + 1 < hostport.size() && hostport[end + 1] == ':') {
                out.port = hostport.substr(end + 2);
            }
            return out;
        }
    }

    auto colon = hostport.rfind(':');
    if (colon != std::string::npos &&
        hostport.find(':') == colon) {
        out.host = hostport.substr(0, colon);
        out.port = hostport.substr(colon + 1);
    } else {
        out.host = hostport;
    }
    return out;
}

OriginParts parse_origin(const std::string& origin) {
    OriginParts out;
    auto scheme_pos = origin.find("://");
    if (scheme_pos == std::string::npos) return out;
    out.scheme = ascii_lower(origin.substr(0, scheme_pos));
    auto rest = origin.substr(scheme_pos + 3);
    auto slash = rest.find('/');
    if (slash != std::string::npos) rest = rest.substr(0, slash);
    OriginParts hp = split_host_port(rest);
    out.host = std::move(hp.host);
    out.port = std::move(hp.port);
    if (out.port.empty()) {
        if (out.scheme == "http") out.port = "80";
        else if (out.scheme == "https") out.port = "443";
    }
    return out;
}

} // namespace acecode::web
