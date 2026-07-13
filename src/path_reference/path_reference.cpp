#include "path_reference.hpp"

#include "web/handlers/files_handler.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <variant>

namespace acecode::path_reference {

namespace {

bool is_ascii_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string normalize_slashes(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    while (value.size() > 1 && value.find("//") != std::string::npos) {
        value.erase(value.find("//"), 1);
    }
    if (value.rfind("./", 0) == 0) value.erase(0, 2);
    return value;
}

std::string ascii_lower(std::string value) {
    for (char& c : value) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x80) c = static_cast<char>(std::tolower(u));
    }
    return value;
}

bool unsafe_relative_path(const std::string& value) {
    const std::string path = normalize_slashes(value);
    if (path.empty()) return false;
    if (path.front() == '/' || path.front() == '\\') return true;
    if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) &&
        path[1] == ':') {
        return true;
    }
    std::size_t start = 0;
    while (start <= path.size()) {
        const auto slash = path.find('/', start);
        const auto part = path.substr(start, slash == std::string::npos
            ? std::string::npos : slash - start);
        if (part == "..") return true;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return false;
}

bool contains_ascii_whitespace(const std::string& value) {
    return std::any_of(value.begin(), value.end(), is_ascii_space);
}

} // namespace

std::optional<Token> token_at_cursor(const std::string& input,
                                     std::size_t cursor_bytes) {
    cursor_bytes = std::min(cursor_bytes, input.size());
    for (std::size_t at = cursor_bytes + 1; at-- > 0;) {
        if (at >= input.size() || input[at] != '@') continue;
        if (at > 0 && !is_ascii_space(input[at - 1])) continue;

        const bool quoted = at + 1 < input.size() && input[at + 1] == '"';
        std::size_t end = at + (quoted ? 2 : 1);
        if (quoted) {
            while (end < input.size() && input[end] != '"' &&
                   input[end] != '\r' && input[end] != '\n') {
                ++end;
            }
            if (end < input.size() && input[end] == '"') ++end;
        } else {
            while (end < input.size() && !is_ascii_space(input[end])) ++end;
        }
        if (cursor_bytes < at || cursor_bytes > end) continue;

        std::size_t value_begin = at + (quoted ? 2 : 1);
        std::size_t value_end = end;
        if (quoted && value_end > value_begin && input[value_end - 1] == '"') {
            --value_end;
        }
        return Token{at, end,
                     input.substr(value_begin, value_end - value_begin), quoted};
    }
    return std::nullopt;
}

Query split_query(const std::string& path) {
    const std::string normalized = normalize_slashes(path);
    const auto slash = normalized.find_last_of('/');
    if (slash == std::string::npos) return Query{"", normalized};
    return Query{normalized.substr(0, slash), normalized.substr(slash + 1)};
}

std::string format_reference(const std::string& relative_path,
                             bool is_directory,
                             bool trailing_space) {
    std::string path = normalize_slashes(relative_path);
    while (!path.empty() && path.front() == '/') path.erase(path.begin());
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    if (is_directory && (path.empty() || path.back() != '/')) path.push_back('/');
    const bool quote = contains_ascii_whitespace(path);
    std::string out = quote ? "@\"" + path + "\"" : "@" + path;
    if (trailing_space) out.push_back(' ');
    return out;
}

Replacement replace_token(const std::string& input,
                          const Token& token,
                          const std::string& relative_path,
                          bool is_directory,
                          bool enter_directory) {
    const std::size_t begin = std::min(token.begin, input.size());
    const std::size_t end = std::clamp(token.end, begin, input.size());
    const std::string replacement = format_reference(
        relative_path, is_directory, !enter_directory);
    Replacement out;
    out.text.reserve(input.size() - (end - begin) + replacement.size());
    out.text.append(input, 0, begin);
    out.text += replacement;
    out.text.append(input, end, std::string::npos);
    out.cursor = begin + replacement.size();
    return out;
}

SuggestionResult suggest(const std::string& cwd, const std::string& typed_path) {
    SuggestionResult out;
    if (cwd.empty()) {
        out.error = "working directory unavailable";
        return out;
    }
    if (unsafe_relative_path(typed_path)) {
        out.error = "path outside working directory";
        return out;
    }

    const Query query = split_query(typed_path);
    const std::vector<std::string> allowed{cwd};
    auto root_result = web::validate_path_within(cwd, "", allowed);
    auto dir_result = web::validate_path_within(cwd, query.directory, allowed);
    if (std::holds_alternative<web::FileError>(root_result) ||
        std::holds_alternative<web::FileError>(dir_result)) {
        out.error = "path outside working directory";
        return out;
    }

    const auto& root = std::get<std::filesystem::path>(root_result);
    const auto& dir = std::get<std::filesystem::path>(dir_result);
    auto listed = web::list_directory(dir, root, true, true);
    if (std::holds_alternative<web::FileError>(listed)) {
        out.error = std::get<web::FileError>(listed).message;
        if (out.error.empty()) out.error = "directory unavailable";
        return out;
    }

    const std::string needle = ascii_lower(query.filter);
    for (const auto& entry : std::get<std::vector<web::FileEntry>>(listed)) {
        if (!needle.empty() && ascii_lower(entry.name).find(needle) == std::string::npos) {
            continue;
        }
        out.items.push_back(Candidate{entry.name, normalize_slashes(entry.path),
                                      entry.kind == "dir"});
    }
    std::sort(out.items.begin(), out.items.end(), [](const Candidate& a,
                                                     const Candidate& b) {
        if (a.is_directory != b.is_directory) return a.is_directory;
        const auto al = ascii_lower(a.name);
        const auto bl = ascii_lower(b.name);
        if (al != bl) return al < bl;
        return a.name < b.name;
    });
    if (out.items.size() > kMaxSuggestions) out.items.resize(kMaxSuggestions);
    return out;
}

} // namespace acecode::path_reference
