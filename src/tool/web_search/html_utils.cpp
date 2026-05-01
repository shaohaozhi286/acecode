#include "html_utils.hpp"

#include <cctype>
#include <cstdint>
#include <string>

namespace acecode::web_search {

namespace {

// 将 Unicode codepoint 编码为 UTF-8 追加到 out。out-of-range / 代理对原样丢弃。
void append_utf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        if (cp >= 0xD800 && cp <= 0xDFFF) return; // 代理对不接受
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// 给定 in[i+1..] 起始的 entity 内容(不含起头的 '&'),如果是已知命名实体,
// 返回对应 UTF-8 字节并把 i 推进到 ';' 之后;否则返回空字符串保持 i 不动。
std::string try_named_entity(std::string_view in, std::size_t& i) {
    static constexpr struct { const char* name; const char* utf8; } kTable[] = {
        {"amp",  "&"},
        {"lt",   "<"},
        {"gt",   ">"},
        {"quot", "\""},
        {"apos", "'"},
        {"nbsp", " "}, // U+00A0,统一收成普通空格(后续 collapse_whitespace 会再处理)
    };
    for (const auto& e : kTable) {
        std::size_t name_len = 0;
        while (e.name[name_len] != '\0') ++name_len;
        if (i + 1 + name_len < in.size() &&
            in.compare(i + 1, name_len, e.name) == 0 &&
            in[i + 1 + name_len] == ';') {
            i += 1 + name_len + 1; // 跳过 '&', name, ';'
            return std::string(e.utf8);
        }
    }
    return {};
}

// 给定 in[i] == '&' 且 in[i+1] == '#',尝试解析数字字符引用。成功返回 UTF-8
// 串并推进 i;失败保持 i 不动并返回空。
std::string try_numeric_entity(std::string_view in, std::size_t& i) {
    if (i + 2 >= in.size() || in[i] != '&' || in[i + 1] != '#') return {};
    std::size_t j = i + 2;
    bool hex = false;
    if (j < in.size() && (in[j] == 'x' || in[j] == 'X')) {
        hex = true;
        ++j;
    }
    std::size_t start = j;
    uint32_t cp = 0;
    while (j < in.size() && in[j] != ';') {
        char c = in[j];
        if (hex) {
            if (c >= '0' && c <= '9') cp = cp * 16 + (c - '0');
            else if (c >= 'a' && c <= 'f') cp = cp * 16 + (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') cp = cp * 16 + (c - 'A' + 10);
            else return {};
        } else {
            if (c >= '0' && c <= '9') cp = cp * 10 + (c - '0');
            else return {};
        }
        if (cp > 0x10FFFF) return {}; // 越界保护
        ++j;
    }
    if (j == start || j >= in.size() || in[j] != ';') return {};
    i = j + 1;
    std::string out;
    append_utf8(out, cp);
    return out;
}

bool is_ws(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

// 计数 UTF-8 串首字节,返回该字符的字节长度(1..4)。无效字节当 1 处理。
int utf8_seq_len(unsigned char b) {
    if ((b & 0x80) == 0x00) return 1;
    if ((b & 0xE0) == 0xC0) return 2;
    if ((b & 0xF0) == 0xE0) return 3;
    if ((b & 0xF8) == 0xF0) return 4;
    return 1;
}

} // namespace

std::string html_decode_entities(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    std::size_t i = 0;
    while (i < in.size()) {
        if (in[i] != '&') {
            out.push_back(in[i++]);
            continue;
        }
        // 尝试数字引用 (&#NN; 或 &#xHH;)
        if (i + 2 < in.size() && in[i + 1] == '#') {
            std::size_t saved = i;
            std::string decoded = try_numeric_entity(in, i);
            if (!decoded.empty()) {
                out += decoded;
                continue;
            }
            i = saved;
        }
        // 尝试命名实体
        std::size_t saved = i;
        std::string decoded = try_named_entity(in, i);
        if (!decoded.empty()) {
            out += decoded;
            continue;
        }
        i = saved;
        out.push_back(in[i++]);
    }
    return out;
}

std::string collapse_whitespace(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    bool in_ws = false;
    for (std::size_t i = 0; i < in.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(in[i]);
        if (is_ws(c)) {
            if (!out.empty() && !in_ws) {
                out.push_back(' ');
                in_ws = true;
            }
            // 起始处的空白被跳过(因为 out 还是空)
        } else {
            out.push_back(in[i]);
            in_ws = false;
        }
    }
    // trim 末尾可能的空格
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string truncate_with_ellipsis(std::string_view in, std::size_t max_codepoints) {
    if (max_codepoints == 0) return std::string();
    std::size_t i = 0;
    std::size_t cps = 0;
    while (i < in.size() && cps < max_codepoints) {
        int len = utf8_seq_len(static_cast<unsigned char>(in[i]));
        if (i + static_cast<std::size_t>(len) > in.size()) break;
        i += static_cast<std::size_t>(len);
        ++cps;
    }
    if (i >= in.size()) {
        return std::string(in);
    }
    std::string out;
    out.reserve(i + 3);
    out.append(in.data(), i);
    out.append("\xE2\x80\xA6"); // U+2026 HORIZONTAL ELLIPSIS
    return out;
}

} // namespace acecode::web_search
