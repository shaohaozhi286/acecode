#include "diff_view_truncate.hpp"

#include <algorithm>
#include <string>

namespace acecode {

TruncatedDiff truncate_hunks_for_fold(
    const std::vector<DiffHunk>& hunks,
    int max_hunks,
    int max_lines_per_hunk
) {
    TruncatedDiff out;

    // 先按 hunk 数截。
    size_t take = hunks.size();
    if (max_hunks > 0 && hunks.size() > static_cast<size_t>(max_hunks)) {
        take = static_cast<size_t>(max_hunks);
        out.hidden_hunks = static_cast<int>(hunks.size() - take);
    }

    out.hunks.reserve(take);
    out.hidden_lines_per_hunk.reserve(take);

    for (size_t i = 0; i < take; ++i) {
        DiffHunk h = hunks[i]; // 拷贝,后续可能截 lines
        int hidden_lines = 0;

        if (max_lines_per_hunk > 0 &&
            h.lines.size() > static_cast<size_t>(max_lines_per_hunk)) {
            // 保留前一半 + 后一半,中间丢弃。前一半向上取整,让前后对称或前侧多 1。
            const int keep = max_lines_per_hunk;
            const int head = (keep + 1) / 2;
            const int tail = keep - head;
            hidden_lines = static_cast<int>(h.lines.size()) - keep;

            std::vector<DiffLine> trimmed;
            trimmed.reserve(static_cast<size_t>(keep));
            for (int k = 0; k < head; ++k) {
                trimmed.push_back(h.lines[static_cast<size_t>(k)]);
            }
            for (int k = 0; k < tail; ++k) {
                size_t src_idx = h.lines.size() - static_cast<size_t>(tail - k);
                trimmed.push_back(h.lines[src_idx]);
            }
            h.lines = std::move(trimmed);
        }

        out.hunks.push_back(std::move(h));
        out.hidden_lines_per_hunk.push_back(hidden_lines);
    }

    return out;
}

std::string format_gutter(
    DiffLineKind kind,
    const std::optional<int>& old_line_no,
    const std::optional<int>& new_line_no,
    int line_no_width
) {
    if (line_no_width < 1) line_no_width = 1;

    // 选择显示哪一侧的行号:Added 显示 new,Removed 显示 old,Context 优先
    // 显示 new(与"当前文件视角"一致,和 claudecodehaha 的 Fallback 顺序一致)。
    std::optional<int> shown;
    char marker = ' ';
    switch (kind) {
        case DiffLineKind::Added:
            marker = '+';
            shown = new_line_no;
            break;
        case DiffLineKind::Removed:
            marker = '-';
            shown = old_line_no;
            break;
        case DiffLineKind::Context:
            marker = ' ';
            shown = new_line_no.has_value() ? new_line_no : old_line_no;
            break;
    }

    std::string num_str;
    if (shown.has_value()) {
        num_str = std::to_string(*shown);
    }
    // 右对齐到 line_no_width
    if (static_cast<int>(num_str.size()) < line_no_width) {
        num_str = std::string(static_cast<size_t>(line_no_width) - num_str.size(), ' ') + num_str;
    }

    std::string out;
    out.reserve(num_str.size() + 2);
    out += num_str;
    out += ' ';
    out += marker;
    return out;
}

int compute_line_no_width(const DiffHunk& hunk) {
    int max_no = 1;
    int old_end = hunk.old_start + hunk.old_count - 1;
    int new_end = hunk.new_start + hunk.new_count - 1;
    if (old_end > max_no) max_no = old_end;
    if (new_end > max_no) max_no = new_end;
    int w = 1;
    while (max_no >= 10) {
        max_no /= 10;
        ++w;
    }
    return w;
}

} // namespace acecode
