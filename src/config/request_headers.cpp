#include "request_headers.hpp"

#include "../utils/encoding.hpp"

#include <algorithm>
#include <cctype>
#include <set>

namespace acecode {
namespace {

bool is_tchar(unsigned char ch) {
    if (std::isalnum(ch)) return true;
    switch (ch) {
        case '!': case '#': case '$': case '%': case '&': case '\'':
        case '*': case '+': case '-': case '.': case '^': case '_':
        case '`': case '|': case '~':
            return true;
        default:
            return false;
    }
}

std::string ascii_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool contains_control(const std::string& value) {
    for (unsigned char ch : value) {
        if (std::iscntrl(ch)) return true;
    }
    return false;
}

bool valid_env_name(const std::string& name) {
    if (name.empty()) return false;
    for (unsigned char ch : name) {
        if (!(std::isalnum(ch) || ch == '_')) return false;
    }
    return true;
}

bool validate_template(const std::string& value, std::string& err) {
    for (std::size_t pos = 0; pos < value.size();) {
        const std::size_t open = value.find("{env:", pos);
        if (open == std::string::npos) {
            if (value.find('}', pos) != std::string::npos) {
                err = "request_headers contains malformed env placeholder";
                return false;
            }
            return true;
        }
        if (value.find('}', pos) != std::string::npos &&
            value.find('}', pos) < open) {
            err = "request_headers contains malformed env placeholder";
            return false;
        }
        const std::size_t close = value.find('}', open + 5);
        if (close == std::string::npos) {
            err = "request_headers contains malformed env placeholder";
            return false;
        }
        std::string name = value.substr(open + 5, close - (open + 5));
        if (!valid_env_name(name)) {
            err = "request_headers contains malformed env placeholder";
            return false;
        }
        pos = close + 1;
    }
    return true;
}

std::optional<std::string> resolve_template(const std::string& value, std::string& err) {
    std::string out;
    out.reserve(value.size());
    for (std::size_t pos = 0; pos < value.size();) {
        const std::size_t open = value.find("{env:", pos);
        if (open == std::string::npos) {
            out.append(value.substr(pos));
            return out;
        }
        out.append(value.substr(pos, open - pos));
        const std::size_t close = value.find('}', open + 5);
        if (close == std::string::npos) {
            err = "malformed env placeholder in request_headers";
            return std::nullopt;
        }
        const std::string name = value.substr(open + 5, close - (open + 5));
        std::string env_value;
        if (!getenv_utf8(name.c_str(), env_value)) {
            err = "missing environment variable '" + name + "' for request_headers";
            return std::nullopt;
        }
        out.append(env_value);
        pos = close + 1;
    }
    return out;
}

} // namespace

std::optional<RequestHeaders> parse_request_headers_json(const nlohmann::json& node,
                                                         const std::string& context,
                                                         std::string& err) {
    if (!node.is_object()) {
        err = context + " request_headers must be an object";
        return std::nullopt;
    }

    RequestHeaders out;
    for (auto it = node.begin(); it != node.end(); ++it) {
        if (!it.value().is_string()) {
            err = context + " request_headers values must be strings";
            return std::nullopt;
        }
        out[it.key()] = it.value().get<std::string>();
    }
    return out;
}

bool validate_request_headers(const RequestHeaders& headers,
                              std::string& err,
                              bool allow_authorization) {
    std::set<std::string> seen_lower;
    for (const auto& [name, value] : headers) {
        if (name.empty()) {
            err = "request_headers contains empty header name";
            return false;
        }
        for (unsigned char ch : name) {
            if (!is_tchar(ch)) {
                err = "request_headers contains invalid header name '" + name + "'";
                return false;
            }
        }
        const std::string lower = ascii_lower(name);
        if (!seen_lower.insert(lower).second) {
            err = "request_headers contains duplicate header name '" + name + "'";
            return false;
        }
        if (lower == "content-type") {
            err = "request_headers cannot override Content-Type";
            return false;
        }
        if (!allow_authorization && lower == "authorization") {
            err = "request_headers cannot override Authorization";
            return false;
        }
        if (contains_control(value)) {
            err = "request_headers contains invalid value for '" + name + "'";
            return false;
        }
        if (!validate_template(value, err)) {
            return false;
        }
    }
    return true;
}

std::optional<RequestHeaders> resolve_request_headers(const RequestHeaders& headers,
                                                      std::string& err) {
    RequestHeaders out;
    for (const auto& [name, value] : headers) {
        auto resolved = resolve_template(value, err);
        if (!resolved.has_value()) return std::nullopt;
        out[name] = std::move(*resolved);
    }
    return out;
}

} // namespace acecode
