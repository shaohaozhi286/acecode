// acetui/src/internal/utf8.hpp — 仅供 acetui 实现使用的 UTF-8 小工具。
// 不暴露给 acetui 库的用户(放在 src/ 下而不是 include/)。

#pragma once

#include <string>
#include <vector>

namespace acetui::internal {

// 删除字符串末尾最后一个 UTF-8 codepoint。空字符串无副作用。
inline void utf8_pop_codepoint(std::string& s) {
    while (!s.empty() &&
           (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) {
        s.pop_back();
    }
    if (!s.empty()) {
        s.pop_back();
    }
}

// 把一个 Unicode codepoint 编码成 UTF-8 字节追加到字符串末尾。
// 越界 codepoint(>= 0x110000)无副作用。
inline void utf8_append(std::string& s, char32_t cp) {
    if (cp < 0x80u) {
        s.push_back(static_cast<char>(cp));
    } else if (cp < 0x800u) {
        s.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        s.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x10000u) {
        s.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        s.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        s.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp < 0x110000u) {
        s.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        s.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        s.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        s.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

// UTF-8 字符串的近似显示宽度(不读完整 wcwidth 表):
//   ASCII = 1 列;2-byte sequence = 1;3/4-byte sequence = 2(把 CJK 当宽 2)。
inline int utf8_display_width(const std::string& s) {
    int w = 0;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            w += 1;
            i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            w += 1;
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            w += 2;
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            w += 2;
            i += 4;
        } else {
            i += 1;
        }
    }
    return w;
}

// 单 codepoint 的字节长度(根据 UTF-8 lead byte)。非法 lead 返 1。
inline int utf8_codepoint_byte_len(unsigned char lead) {
    if (lead < 0x80)            return 1;
    if ((lead & 0xE0) == 0xC0)  return 2;
    if ((lead & 0xF0) == 0xE0)  return 3;
    if ((lead & 0xF8) == 0xF0)  return 4;
    return 1;
}

// pos 处 codepoint 的字节长度。pos >= s.size() 返 0。
inline int utf8_codepoint_len_at(const std::string& s, size_t pos) {
    if (pos >= s.size()) return 0;
    return utf8_codepoint_byte_len(static_cast<unsigned char>(s[pos]));
}

// pos 之前那个 codepoint 的字节长度(往回走过 continuation byte)。
// pos == 0 返 0。pos > s.size() 视作 s.size()。
inline int utf8_codepoint_len_before(const std::string& s, size_t pos) {
    if (pos == 0) return 0;
    if (pos > s.size()) pos = s.size();
    size_t i = pos;
    int len  = 0;
    while (i > 0) {
        --i;
        ++len;
        unsigned char c = static_cast<unsigned char>(s[i]);
        if ((c & 0xC0) != 0x80) {
            // 非 continuation 字节 = lead byte,停。
            return len;
        }
        if (len >= 4) {
            // 防御性:UTF-8 最长 4 字节,再走更多就是非法序列,当 1 字节处理。
            return 1;
        }
    }
    return len;
}

// 单 codepoint 的显示宽度。简化:ASCII = 1;2-byte = 1;3/4-byte = 2。
inline int utf8_codepoint_display_width(unsigned char lead) {
    if (lead < 0x80)            return 1;
    if ((lead & 0xE0) == 0xC0)  return 1;
    if ((lead & 0xF0) == 0xE0)  return 2;
    if ((lead & 0xF8) == 0xF0)  return 2;
    return 1;
}

// 把 UTF-8 字符串按显示宽度切成多段(每段 display_width <= max_width)。
// 不做单词级断行 — 简单按 codepoint 边界硬切,够 chat composer 用。
// max_width <= 0 或空字符串 → 返回单个空段(避免 caller 拿到空 vector)。
inline std::vector<std::string> utf8_wrap(const std::string& s, int max_width) {
    std::vector<std::string> out;
    if (max_width <= 0) {
        out.push_back(s);
        return out;
    }
    if (s.empty()) {
        out.push_back("");
        return out;
    }

    std::string cur;
    int cur_w = 0;
    for (size_t i = 0; i < s.size();) {
        unsigned char c   = static_cast<unsigned char>(s[i]);
        int len           = utf8_codepoint_byte_len(c);
        int w             = utf8_codepoint_display_width(c);
        if (cur_w + w > max_width && !cur.empty()) {
            out.push_back(std::move(cur));
            cur.clear();
            cur_w = 0;
        }
        cur.append(s, i, static_cast<size_t>(len));
        cur_w += w;
        i += static_cast<size_t>(len);
    }
    out.push_back(std::move(cur));
    return out;
}

}  // namespace acetui::internal
