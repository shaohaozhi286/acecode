#pragma once

#include "tool/diff_utils.hpp"

#include <ftxui/dom/elements.hpp>

#include <vector>

namespace acecode {

struct DiffViewOptions {
    int width = 80;               // 渲染可用宽度(由 chat_box.size.x 推导)
    int max_hunks = 3;            // 折叠态最多显示的 hunk 数(<=0 表无限)
    int max_lines_per_hunk = 20;  // 折叠态每 hunk 最多行数(<=0 表无限)
    bool expanded = false;        // 由 Ctrl+E 切换;true 时忽略上面两个上限
    double word_diff_threshold = 0.4; // 低于此值才启用词级深色高亮
};

// 把结构化 hunk 渲染为彩色 diff 块:左 gutter(行号 + marker)+ 右内容
// (满宽绿/红 bgcolor,Added/Removed 配对行可能叠加词级深色高亮)。
ftxui::Element render_diff_view(
    const std::vector<DiffHunk>& hunks,
    const DiffViewOptions& opts
);

} // namespace acecode
