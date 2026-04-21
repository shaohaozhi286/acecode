#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace acecode {

// 词级 diff 段:Same/Added/Removed 三种,对应 claudecodehaha 的
// `diffWordsWithSpace` 结果里 `added` / `removed` / 都无的三态。
enum class WordDiffKind {
    Same,
    Added,
    Removed,
};

struct WordDiffSegment {
    WordDiffKind kind = WordDiffKind::Same;
    std::string value;
};

// 按空白 + ASCII 标点切分得到的 token(保留分隔符本身作为独立 token)。
// 暴露到 header 只为测试方便。
std::vector<std::string> tokenize_for_word_diff(const std::string& s);

// 对两串文本做 token 级 LCS 对齐,返回合并后的段序列。
// 任一侧 token 数 > `max_tokens_per_side` 时早退为 `{Removed a} {Added b}`
// 以防 O(n·m) 在超长 minified 行上卡住 UI。
std::vector<WordDiffSegment> word_diff(
    const std::string& a,
    const std::string& b,
    size_t max_tokens_per_side = 200
);

// 基于词级 diff 结果计算"变化字符占比",用于决定是否启用词级高亮。
// 公式:sum(len(Added) + len(Removed)) / sum(len(all segments));
// 两侧都为空时返回 0.0。
double word_diff_change_ratio(const std::vector<WordDiffSegment>& segments);

// 阈值辅助:默认 ≤ 0.4 算小改,可以叠加词级高亮(与 claudecodehaha
// `CHANGE_THRESHOLD = 0.4` 一致)。
inline bool word_diff_below_threshold(double ratio, double threshold = 0.4) {
    return ratio <= threshold;
}

} // namespace acecode
