#pragma once

#include "diff_utils.hpp"

#include <string>
#include <vector>

namespace acecode {

// 折叠结果:带上被丢掉的 hunk / 行数的元信息,供 TUI 加"N more hunks"提示。
struct TruncatedDiff {
    std::vector<DiffHunk> hunks;
    int hidden_hunks = 0;       // 超出 max_hunks 的 hunk 数
    // 每个保留下来的 hunk,在按行截断后被丢掉的行数。索引与 hunks 对齐。
    std::vector<int> hidden_lines_per_hunk;
};

// 折叠态用的纯函数:
//   - 若 `max_hunks > 0` 且输入超过此数,只保留前 N 个 hunk,`hidden_hunks`
//     统计溢出数量。
//   - 若 `max_lines_per_hunk > 0` 且某个 hunk 的行数超过此阈值,截为
//     "前一半 + 后一半",中间丢掉的行数记到 `hidden_lines_per_hunk`。
//   - `max_hunks <= 0` 或 `max_lines_per_hunk <= 0` 视为无上限(不截断)。
TruncatedDiff truncate_hunks_for_fold(
    const std::vector<DiffHunk>& hunks,
    int max_hunks,
    int max_lines_per_hunk
);

// 渲染 diff 行的 gutter 字符串,格式:"<右对齐行号> <marker>",行号宽度
// 为 `line_no_width`,marker ∈ {' ', '+', '-'};无行号一侧用空格填充。
// 纯函数,便于单测。
std::string format_gutter(
    DiffLineKind kind,
    const std::optional<int>& old_line_no,
    const std::optional<int>& new_line_no,
    int line_no_width
);

// 根据一个 hunk 的行号范围,计算显示时需要的 gutter 行号宽度(至少 1)。
int compute_line_no_width(const DiffHunk& hunk);

} // namespace acecode
