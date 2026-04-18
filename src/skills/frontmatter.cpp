#include "frontmatter.hpp"

#include <cctype>
#include <sstream>

namespace acecode {

namespace {

std::string strip(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(start, end - start);
}

std::string unquote(const std::string& s) {
    if (s.size() >= 2) {
        char first = s.front();
        char last = s.back();
        if ((first == '"' || first == '\'') && first == last) {
            return s.substr(1, s.size() - 2);
        }
    }
    return s;
}

// Count leading spaces (not tabs — YAML disallows tabs for indent but we keep
// behaviour lenient: tabs count as one space each).
int leading_indent(const std::string& line) {
    int n = 0;
    for (char c : line) {
        if (c == ' ' || c == '\t') ++n;
        else break;
    }
    return n;
}

std::vector<std::string> parse_bracket_list(const std::string& value) {
    // Expects the already-stripped content between the brackets, e.g. "a, b, c".
    std::vector<std::string> out;
    std::string current;
    for (char c : value) {
        if (c == ',') {
            std::string item = strip(current);
            if (!item.empty()) out.push_back(unquote(item));
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    std::string item = strip(current);
    if (!item.empty()) out.push_back(unquote(item));
    return out;
}

// Parse a "key: value" split. Returns true with (key, value) filled when the
// line looks like a mapping entry, false otherwise. value may be empty when the
// key introduces a nested block.
bool split_kv(const std::string& line, std::string& key, std::string& value) {
    auto pos = line.find(':');
    if (pos == std::string::npos) return false;
    key = strip(line.substr(0, pos));
    value = strip(line.substr(pos + 1));
    if (key.empty()) return false;
    return true;
}

// Fallback parser: just pull out `key: value` lines with no nesting. Used when
// the structured parser hits something unexpected — we still recover *some*
// metadata rather than dropping the whole skill.
Frontmatter simple_kv_parse(const std::string& yaml) {
    Frontmatter out;
    std::istringstream iss(yaml);
    std::string line;
    while (std::getline(iss, line)) {
        std::string trimmed = strip(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;
        std::string key, value;
        if (!split_kv(trimmed, key, value)) continue;
        // Strip inline bracket lists too — good enough for tags/platforms.
        if (value.size() >= 2 && value.front() == '[' && value.back() == ']') {
            auto items = parse_bracket_list(value.substr(1, value.size() - 2));
            out[key] = FrontmatterValue(items);
        } else if (!value.empty()) {
            out[key] = FrontmatterValue(unquote(value));
        }
    }
    return out;
}

// Forward declaration for recursive map parse.
FrontmatterMap parse_mapping_block(const std::vector<std::string>& lines,
                                   size_t& idx,
                                   int base_indent);

// Parse a value that follows `key:`, potentially spilling onto later lines as
// either a nested map (children more indented) or a dash list. `idx` points at
// the line *after* the key line on entry; on return it points at the first
// line that belongs to a sibling/outer scope.
FrontmatterValue parse_following_block(const std::vector<std::string>& lines,
                                       size_t& idx,
                                       int key_indent) {
    // Peek first non-blank, non-comment line to decide shape.
    while (idx < lines.size()) {
        const std::string& peek = lines[idx];
        std::string trimmed = strip(peek);
        if (trimmed.empty() || trimmed[0] == '#') { ++idx; continue; }

        int indent = leading_indent(peek);
        if (indent <= key_indent) {
            // No indented block — treat as empty string value.
            return FrontmatterValue(std::string{});
        }

        if (trimmed[0] == '-') {
            // Dash-list: consume lines that start with "- " at this indent.
            std::vector<std::string> items;
            while (idx < lines.size()) {
                const std::string& l = lines[idx];
                std::string t = strip(l);
                if (t.empty() || t[0] == '#') { ++idx; continue; }
                int ind = leading_indent(l);
                if (ind <= key_indent) break;
                if (t.size() >= 2 && t[0] == '-') {
                    std::string item = strip(t.substr(1));
                    items.push_back(unquote(item));
                    ++idx;
                } else {
                    break;
                }
            }
            return FrontmatterValue(items);
        }

        // Nested mapping: parse until indent returns to <= key_indent.
        FrontmatterMap m = parse_mapping_block(lines, idx, indent);
        return FrontmatterValue(m);
    }
    return FrontmatterValue(std::string{});
}

FrontmatterMap parse_mapping_block(const std::vector<std::string>& lines,
                                   size_t& idx,
                                   int base_indent) {
    FrontmatterMap out;
    while (idx < lines.size()) {
        const std::string& line = lines[idx];
        std::string trimmed = strip(line);
        if (trimmed.empty() || trimmed[0] == '#') { ++idx; continue; }

        int indent = leading_indent(line);
        if (indent < base_indent) break;
        if (indent > base_indent) {
            // Unexpected extra indent — skip to avoid infinite loop.
            ++idx;
            continue;
        }

        std::string key, value;
        if (!split_kv(trimmed, key, value)) {
            ++idx;
            continue;
        }
        ++idx;

        if (value.empty()) {
            out[key] = parse_following_block(lines, idx, indent);
        } else if (value.size() >= 2 && value.front() == '[' && value.back() == ']') {
            auto items = parse_bracket_list(value.substr(1, value.size() - 2));
            out[key] = FrontmatterValue(items);
        } else {
            out[key] = FrontmatterValue(unquote(value));
        }
    }
    return out;
}

} // namespace

std::pair<Frontmatter, std::string> parse_frontmatter(const std::string& content) {
    // Must start with "---" optionally followed by whitespace then newline.
    if (content.size() < 4 || content.compare(0, 3, "---") != 0) {
        return {Frontmatter{}, content};
    }
    // Find the terminating "\n---" line.
    size_t search_pos = 3;
    // Skip past the opening delimiter line.
    size_t first_nl = content.find('\n', 0);
    if (first_nl == std::string::npos) return {Frontmatter{}, content};
    search_pos = first_nl + 1;

    // Look for a line whose content is exactly "---" (optionally trailing ws).
    size_t end_pos = std::string::npos;
    size_t cursor = search_pos;
    while (cursor < content.size()) {
        size_t line_end = content.find('\n', cursor);
        std::string line = (line_end == std::string::npos)
            ? content.substr(cursor)
            : content.substr(cursor, line_end - cursor);
        std::string t = strip(line);
        if (t == "---") {
            end_pos = cursor;
            break;
        }
        if (line_end == std::string::npos) break;
        cursor = line_end + 1;
    }

    if (end_pos == std::string::npos) {
        return {Frontmatter{}, content};
    }

    std::string yaml_block = content.substr(search_pos, end_pos - search_pos);
    // Body starts after the closing "---\n".
    size_t body_start = content.find('\n', end_pos);
    std::string body = (body_start == std::string::npos)
        ? std::string{}
        : content.substr(body_start + 1);

    // Split into lines for structured pass.
    std::vector<std::string> lines;
    {
        std::istringstream iss(yaml_block);
        std::string l;
        while (std::getline(iss, l)) lines.push_back(l);
    }

    Frontmatter fm;
    try {
        size_t idx = 0;
        fm = parse_mapping_block(lines, idx, 0);
    } catch (...) {
        fm = simple_kv_parse(yaml_block);
    }

    if (fm.empty()) {
        fm = simple_kv_parse(yaml_block);
    }

    return {fm, body};
}

std::string get_string(const Frontmatter& fm, const std::string& key,
                       const std::string& fallback) {
    auto it = fm.find(key);
    if (it == fm.end()) return fallback;
    if (it->second.is_string()) return it->second.string_value;
    if (it->second.is_list() && !it->second.list_value.empty()) {
        return it->second.list_value.front();
    }
    return fallback;
}

std::vector<std::string> get_list(const Frontmatter& fm, const std::string& key) {
    auto it = fm.find(key);
    if (it == fm.end()) return {};
    if (it->second.is_list()) return it->second.list_value;
    if (it->second.is_string() && !it->second.string_value.empty()) {
        return {it->second.string_value};
    }
    return {};
}

const FrontmatterValue* get_nested(const Frontmatter& fm,
                                   const std::vector<std::string>& path) {
    if (path.empty()) return nullptr;
    const FrontmatterMap* current = &fm;
    const FrontmatterValue* result = nullptr;
    for (size_t i = 0; i < path.size(); ++i) {
        auto it = current->find(path[i]);
        if (it == current->end()) return nullptr;
        result = &it->second;
        if (i + 1 < path.size()) {
            if (!result->is_map()) return nullptr;
            current = &result->map_value;
        }
    }
    return result;
}

} // namespace acecode
