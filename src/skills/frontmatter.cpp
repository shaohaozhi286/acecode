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

// YAML 块标量指示符:`|`(literal,保留换行)/ `>`(folded,换行折叠成空格),
// 可带 chomping 后缀 `-`(strip)或 `+`(keep)。claude-code 生态里大量 skill 用
// `description: >-` 写多行描述 — 不识别时值会变成字面的 ">-",后续行按
// "意外缩进" 被丢弃(历史 bug:设置页里 description 只显示 ">" 或 "|")。
bool is_block_scalar_indicator(const std::string& value, char& style) {
    if (value.empty()) return false;
    if (value[0] != '|' && value[0] != '>') return false;
    style = value[0];
    // 只接受 "|", ">", "|-", ">-", "|+", ">+"(可带尾随空白,调用方已 strip)。
    if (value.size() == 1) return true;
    if (value.size() == 2 && (value[1] == '-' || value[1] == '+')) return true;
    return false;
}

// Consume the indented block following a `key: |` / `key: >` line and join it
// into a single string. `idx` points at the line after the key line on entry;
// on return it points at the first line outside the block. Trailing whitespace
// is always stripped (chomping subtleties don't matter for metadata display).
std::string parse_block_scalar(const std::vector<std::string>& lines,
                               size_t& idx,
                               int key_indent,
                               char style) {
    // 块的基准缩进 = 第一条非空行的缩进;空行属于块内容(段落分隔)。
    int base_indent = -1;
    std::vector<std::string> block;
    while (idx < lines.size()) {
        const std::string& line = lines[idx];
        std::string trimmed = strip(line);
        if (trimmed.empty()) {
            // 空行:先暂存,块结束时尾部空行会被 strip 掉。
            block.push_back("");
            ++idx;
            continue;
        }
        int indent = leading_indent(line);
        if (indent <= key_indent) break;
        if (base_indent < 0) base_indent = indent;
        block.push_back(line.size() > static_cast<size_t>(base_indent)
                            ? line.substr(base_indent)
                            : std::string{});
        ++idx;
    }
    // 去掉尾部空行(它们可能属于块后面的空白,而不是内容)。
    while (!block.empty() && strip(block.back()).empty()) block.pop_back();

    std::string out;
    if (style == '|') {
        for (size_t i = 0; i < block.size(); ++i) {
            if (i) out += '\n';
            out += block[i];
        }
    } else {
        // folded:相邻非空行以空格连接,空行折叠成一个换行。
        bool prev_blank = true; // 开头不补分隔
        for (const auto& l : block) {
            if (strip(l).empty()) {
                out += '\n';
                prev_blank = true;
                continue;
            }
            if (!prev_blank) out += ' ';
            out += l;
            prev_blank = false;
        }
    }
    // strip 尾白;chomping(- / +)只影响尾换行数量,对元数据展示无意义。
    while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back()))) {
        out.pop_back();
    }
    return out;
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

        char block_style = 0;
        if (value.empty()) {
            out[key] = parse_following_block(lines, idx, indent);
        } else if (is_block_scalar_indicator(value, block_style)) {
            out[key] = FrontmatterValue(
                parse_block_scalar(lines, idx, indent, block_style));
        } else if (value.size() >= 2 && value.front() == '[' && value.back() == ']') {
            auto items = parse_bracket_list(value.substr(1, value.size() - 2));
            out[key] = FrontmatterValue(items);
        } else {
            out[key] = FrontmatterValue(unquote(value));
        }
    }
    return out;
}

Frontmatter parse_yaml_block(const std::string& yaml_block) {
    std::vector<std::string> lines;
    {
        std::istringstream iss(yaml_block);
        std::string l;
        while (std::getline(iss, l)) {
            // CRLF 文件:getline 按 \n 分割会留下行尾 \r。普通 value 走 strip
            // 时被顺带去掉,但块标量按原始行取内容,\r 会混进 description
            // (实测 pdf skill 的描述里出现 "\r ")。统一在分行处去掉。
            if (!l.empty() && l.back() == '\r') l.pop_back();
            lines.push_back(l);
        }
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

    return fm;
}

bool has_skill_metadata_shape(const Frontmatter& fm) {
    return fm.find("name") != fm.end() || fm.find("description") != fm.end();
}

} // namespace

std::pair<Frontmatter, std::string> parse_frontmatter(const std::string& content) {
    // Must start with "---" optionally followed by whitespace then newline.
    if (content.size() < 4 || content.compare(0, 3, "---") != 0) {
        // Legacy ACECode skills were sometimes written as:
        //
        //   name: ...
        //   description: ...
        //   ---
        //   # body
        //
        // Accept that shape for compatibility, but only when the header parses
        // into recognizable skill metadata.
        size_t cursor = 0;
        while (cursor < content.size()) {
            size_t line_end = content.find('\n', cursor);
            std::string line = (line_end == std::string::npos)
                ? content.substr(cursor)
                : content.substr(cursor, line_end - cursor);
            if (strip(line) == "---") {
                std::string yaml_block = content.substr(0, cursor);
                Frontmatter fm = parse_yaml_block(yaml_block);
                if (has_skill_metadata_shape(fm)) {
                    std::string body = (line_end == std::string::npos)
                        ? std::string{}
                        : content.substr(line_end + 1);
                    return {fm, body};
                }
                break;
            }
            if (line_end == std::string::npos) break;
            cursor = line_end + 1;
        }
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

    Frontmatter fm = parse_yaml_block(yaml_block);

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
