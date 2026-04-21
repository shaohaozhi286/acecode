// 覆盖 src/tool/diff_view_truncate.cpp 的纯函数 — 折叠态的 hunk 截断
// 与 gutter 文本格式化。FTXUI 渲染(src/tui/diff_view.cpp)受 CLAUDE.md
// 约束不走单测,这里只验证数据层。
//
// 场景:
//   1. 按 hunk 数截断,hidden_hunks 正确
//   2. 按每 hunk 行数截断:保留前后各半,记录 hidden_lines
//   3. 两个上限都设置为 0 时不截断(视为无限)
//   4. format_gutter:Added / Removed / Context 三种 marker 与行号选择
//   5. format_gutter 对齐宽度,空行号侧用空格填充
//   6. compute_line_no_width:根据 hunk.*_start/count 的最大行号推导宽度

#include <gtest/gtest.h>

#include "tool/diff_view_truncate.hpp"

using acecode::compute_line_no_width;
using acecode::DiffHunk;
using acecode::DiffLine;
using acecode::DiffLineKind;
using acecode::format_gutter;
using acecode::truncate_hunks_for_fold;

// 构造 helper:生成一个含有 `n` 行 Context 的 hunk(仅用于截断场景)。
static DiffHunk make_hunk_with_n_lines(int n) {
    DiffHunk h;
    h.old_start = 1;
    h.new_start = 1;
    h.old_count = n;
    h.new_count = n;
    for (int i = 0; i < n; ++i) {
        DiffLine l;
        l.kind = DiffLineKind::Context;
        l.text = "line" + std::to_string(i);
        l.old_line_no = i + 1;
        l.new_line_no = i + 1;
        h.lines.push_back(std::move(l));
    }
    return h;
}

// 场景 1: 5 个 hunk + max_hunks=3 → 保留 3 个,hidden_hunks=2。
TEST(TruncateHunks, LimitsHunkCount) {
    std::vector<DiffHunk> hunks;
    for (int i = 0; i < 5; ++i) hunks.push_back(make_hunk_with_n_lines(2));
    auto td = truncate_hunks_for_fold(hunks, /*max_hunks=*/3, /*max_lines_per_hunk=*/0);
    EXPECT_EQ(td.hunks.size(), 3u);
    EXPECT_EQ(td.hidden_hunks, 2);
}

// 场景 2: 单 hunk 20 行 + max_lines_per_hunk=10 →
// 保留前一半 + 后一半(共 10),hidden_lines=10。
TEST(TruncateHunks, LimitsLinesPerHunk) {
    auto h = make_hunk_with_n_lines(20);
    auto td = truncate_hunks_for_fold({h}, /*max_hunks=*/0, /*max_lines_per_hunk=*/10);
    ASSERT_EQ(td.hunks.size(), 1u);
    EXPECT_EQ(td.hunks[0].lines.size(), 10u);
    ASSERT_EQ(td.hidden_lines_per_hunk.size(), 1u);
    EXPECT_EQ(td.hidden_lines_per_hunk[0], 10);
    // 首尾行应保留(来自原 hunk 的 0 和 19)
    EXPECT_EQ(td.hunks[0].lines.front().text, "line0");
    EXPECT_EQ(td.hunks[0].lines.back().text, "line19");
}

// 场景 3: 两个上限都 <=0 时等同于关闭折叠,原样返回。
TEST(TruncateHunks, NoLimitsReturnsAsIs) {
    std::vector<DiffHunk> hunks;
    for (int i = 0; i < 5; ++i) hunks.push_back(make_hunk_with_n_lines(2));
    auto td = truncate_hunks_for_fold(hunks, /*max_hunks=*/0, /*max_lines_per_hunk=*/0);
    EXPECT_EQ(td.hunks.size(), 5u);
    EXPECT_EQ(td.hidden_hunks, 0);
    for (int h : td.hidden_lines_per_hunk) {
        EXPECT_EQ(h, 0);
    }
}

// 场景 4: format_gutter marker 与行号选择。
TEST(FormatGutter, MarkerAndLineNo) {
    EXPECT_EQ(format_gutter(DiffLineKind::Added, std::nullopt, 42, 3),
              " 42 +");
    EXPECT_EQ(format_gutter(DiffLineKind::Removed, 7, std::nullopt, 3),
              "  7 -");
    // Context 优先取 new_line_no
    EXPECT_EQ(format_gutter(DiffLineKind::Context, 7, 9, 3),
              "  9  ");
    // Context 没有 new_line_no 时回落到 old_line_no
    EXPECT_EQ(format_gutter(DiffLineKind::Context, 7, std::nullopt, 3),
              "  7  ");
}

// 场景 5: 行号缺失一侧(纯 Added / Removed)时,仍然有合理的宽度填充。
TEST(FormatGutter, AlignmentAcrossKinds) {
    // line_no_width=3 → 行号段 3 字符 + 空格 + marker,总长 5。
    auto g_add = format_gutter(DiffLineKind::Added, std::nullopt, 1, 3);
    EXPECT_EQ(g_add.size(), 5u);
    auto g_rm  = format_gutter(DiffLineKind::Removed, 1, std::nullopt, 3);
    EXPECT_EQ(g_rm.size(), 5u);
    auto g_ctx = format_gutter(DiffLineKind::Context, std::nullopt, std::nullopt, 3);
    // 两侧都无行号:行号段是 3 个空格
    EXPECT_EQ(g_ctx, "     ");
}

// 场景 6: compute_line_no_width 基于 old/new start+count 的最大行号宽度。
TEST(FormatGutter, ComputeLineNoWidth) {
    DiffHunk h1;
    h1.old_start = 1;
    h1.old_count = 5;
    h1.new_start = 1;
    h1.new_count = 5;
    EXPECT_EQ(compute_line_no_width(h1), 1); // max=5 → 1 位

    DiffHunk h2;
    h2.old_start = 100;
    h2.old_count = 5; // old_end = 104
    h2.new_start = 98;
    h2.new_count = 6; // new_end = 103
    EXPECT_EQ(compute_line_no_width(h2), 3);

    DiffHunk h3;
    h3.old_start = 1;
    h3.old_count = 0;
    h3.new_start = 1;
    h3.new_count = 999; // new_end = 999
    EXPECT_EQ(compute_line_no_width(h3), 3);
}
