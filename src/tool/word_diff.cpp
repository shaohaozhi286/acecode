#include "word_diff.hpp"

#include <cctype>
#include <cstddef>

namespace acecode {

namespace {

// 判断一个字节是否属于 token 分隔符类。对 ASCII 范围按空白 + 标点判定;
// 非 ASCII(UTF-8 续字节 / 中日韩等)统一归为非分隔符,让它们作为连续字段
// 存在于某个词 token 里。这对代码 diff 场景已足够——claudecodehaha 的
// `diffWordsWithSpace` 也是简单按空白 + 标点分。
bool is_delimiter_byte(unsigned char c) {
    if (c >= 0x80) return false; // 非 ASCII:不拆
    if (std::isspace(c)) return true;
    if (std::ispunct(c)) return true;
    return false;
}

} // namespace

std::vector<std::string> tokenize_for_word_diff(const std::string& s) {
    std::vector<std::string> tokens;
    std::string current;
    bool current_is_word = false; // 当前累积的 token 属于 word 还是 delimiter

    auto flush = [&]() {
        if (!current.empty()) {
            tokens.push_back(std::move(current));
            current.clear();
        }
    };

    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char ch = static_cast<unsigned char>(s[i]);
        bool delim = is_delimiter_byte(ch);
        if (delim) {
            // 每个分隔符字节独立成一个 token —— 这样连续的两个空格或 "()"
            // 会产生两个 token,和 diffWordsWithSpace 的语义接近。
            flush();
            tokens.emplace_back(1, static_cast<char>(ch));
            current_is_word = false;
        } else {
            if (!current.empty() && !current_is_word) {
                flush();
            }
            current.push_back(static_cast<char>(ch));
            current_is_word = true;
        }
    }
    flush();
    return tokens;
}

std::vector<WordDiffSegment> word_diff(
    const std::string& a,
    const std::string& b,
    size_t max_tokens_per_side
) {
    auto ta = tokenize_for_word_diff(a);
    auto tb = tokenize_for_word_diff(b);

    // 两边都空 → 返回空。
    if (ta.empty() && tb.empty()) {
        return {};
    }

    // 任一侧超出 token 上限:早退为整体 {Removed a}{Added b},不做 LCS。
    if (ta.size() > max_tokens_per_side || tb.size() > max_tokens_per_side) {
        std::vector<WordDiffSegment> out;
        if (!a.empty()) out.push_back({WordDiffKind::Removed, a});
        if (!b.empty()) out.push_back({WordDiffKind::Added, b});
        return out;
    }

    const size_t n = ta.size();
    const size_t m = tb.size();

    // 标准 LCS DP:dp[i][j] = 前 i 个 a token 与 前 j 个 b token 的 LCS 长度。
    std::vector<std::vector<int>> dp(n + 1, std::vector<int>(m + 1, 0));
    for (size_t i = 1; i <= n; ++i) {
        for (size_t j = 1; j <= m; ++j) {
            if (ta[i - 1] == tb[j - 1]) {
                dp[i][j] = dp[i - 1][j - 1] + 1;
            } else {
                dp[i][j] = (dp[i - 1][j] >= dp[i][j - 1]) ? dp[i - 1][j] : dp[i][j - 1];
            }
        }
    }

    // 回溯得到 Same/Added/Removed 序列(此时是逆序)。
    std::vector<WordDiffSegment> rev;
    size_t i = n;
    size_t j = m;
    while (i > 0 && j > 0) {
        if (ta[i - 1] == tb[j - 1]) {
            rev.push_back({WordDiffKind::Same, ta[i - 1]});
            --i;
            --j;
        } else if (dp[i - 1][j] >= dp[i][j - 1]) {
            rev.push_back({WordDiffKind::Removed, ta[i - 1]});
            --i;
        } else {
            rev.push_back({WordDiffKind::Added, tb[j - 1]});
            --j;
        }
    }
    while (i > 0) {
        rev.push_back({WordDiffKind::Removed, ta[i - 1]});
        --i;
    }
    while (j > 0) {
        rev.push_back({WordDiffKind::Added, tb[j - 1]});
        --j;
    }

    // 反转并合并相邻同类段,减少渲染层的 text() 段数。
    std::vector<WordDiffSegment> out;
    for (auto it = rev.rbegin(); it != rev.rend(); ++it) {
        if (!out.empty() && out.back().kind == it->kind) {
            out.back().value += it->value;
        } else {
            out.push_back(*it);
        }
    }
    return out;
}

double word_diff_change_ratio(const std::vector<WordDiffSegment>& segments) {
    size_t total = 0;
    size_t changed = 0;
    for (const auto& s : segments) {
        total += s.value.size();
        if (s.kind != WordDiffKind::Same) {
            changed += s.value.size();
        }
    }
    if (total == 0) return 0.0;
    return static_cast<double>(changed) / static_cast<double>(total);
}

} // namespace acecode
