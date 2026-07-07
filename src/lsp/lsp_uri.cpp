#include "lsp_uri.hpp"

#include <algorithm>
#include <cctype>

namespace acecode::lsp {
namespace {

bool is_unreserved(unsigned char c) {
    return std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
}

char hex_digit(int value) {
    return value < 10 ? static_cast<char>('0' + value)
                      : static_cast<char>('A' + value - 10);
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string percent_encode_path(const std::string& path) {
    std::string out;
    out.reserve(path.size());
    for (unsigned char c : path) {
        // '/' 保留为路径分隔;':' 保留给 Windows 盘符段(file:///c:/...)。
        if (is_unreserved(c) || c == '/' || c == ':') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex_digit((c >> 4) & 0xF));
            out.push_back(hex_digit(c & 0xF));
        }
    }
    return out;
}

std::string percent_decode(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            const int hi = hex_value(text[i + 1]);
            const int lo = hex_value(text[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(text[i]);
    }
    return out;
}

bool starts_with_ci(const std::string& text, const char* prefix) {
    std::size_t i = 0;
    for (; prefix[i] != '\0'; ++i) {
        if (i >= text.size() ||
            std::tolower(static_cast<unsigned char>(text[i])) !=
                std::tolower(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

bool looks_like_windows_drive(const std::string& path) {
    return path.size() >= 2 &&
           std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':';
}

} // namespace

std::string path_to_file_uri(const std::string& utf8_path) {
    std::string normalized = utf8_path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    if (looks_like_windows_drive(normalized)) {
        // 盘符统一小写,与主流 server(clangd/rust-analyzer)的回推形式一致。
        normalized[0] = static_cast<char>(
            std::tolower(static_cast<unsigned char>(normalized[0])));
        return "file:///" + percent_encode_path(normalized);
    }
    if (!normalized.empty() && normalized[0] == '/') {
        return "file://" + percent_encode_path(normalized);
    }
    // 相对路径不该出现;尽力而为。
    return "file:///" + percent_encode_path(normalized);
}

std::optional<std::string> file_uri_to_path(const std::string& uri) {
    if (!starts_with_ci(uri, "file://")) return std::nullopt;
    std::string rest = uri.substr(7);
    // 去掉 authority(通常为空;file://server/share 的 UNC 形式保留主机段)。
    std::string authority;
    if (!rest.empty() && rest[0] != '/') {
        const std::size_t slash = rest.find('/');
        if (slash == std::string::npos) return std::nullopt;
        authority = rest.substr(0, slash);
        rest = rest.substr(slash);
    }
    std::string path = percent_decode(rest);
    if (!authority.empty()) {
        // UNC:file://host/share/x → //host/share/x(转回反斜杠形式)
        path = "//" + authority + path;
    } else if (path.size() >= 3 && path[0] == '/' &&
               std::isalpha(static_cast<unsigned char>(path[1])) && path[2] == ':') {
        path.erase(0, 1); // /c:/foo → c:/foo
    }
#ifdef _WIN32
    std::replace(path.begin(), path.end(), '/', '\\');
#endif
    return path;
}

std::string normalize_path_key(const std::string& utf8_path) {
    std::string key = utf8_path;
    std::replace(key.begin(), key.end(), '\\', '/');
#ifdef _WIN32
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
#endif
    // 折叠 "./" 与结尾斜杠不做;调用方传入的都是绝对路径,保持轻量。
    return key;
}

} // namespace acecode::lsp
