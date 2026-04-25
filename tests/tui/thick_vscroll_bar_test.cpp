// 覆盖 src/tui/thick_vscroll_bar.hpp 中 acecode::tui::y_to_focus 的纯映射契约。
// 这是滚动条拖拽的核心数学:鼠标 y 坐标 + track 几何 → (focus_index, line_offset)。
// 渲染层(decorator)和事件层都要调它,任何边界越界、零行消息退化、track 容量
// 阈值的处理出错,都会导致拖拽时跳行 / 卡顿 / 越界 — 对 UX 是直接观感问题,但
// 对纯函数本身可以脱离 FTXUI 单独验证,这里就只测纯映射,不测 ftxui 渲染。
//
// 与上游 vscroll_indicator 的 2x 子格精度无关:y_to_focus 只在整格颗粒度上
// 工作(终端里鼠标坐标本来就是整格),子格精度只用于绘制拇指边缘。

#include <gtest/gtest.h>

#include "tui/thick_vscroll_bar.hpp"

#include <utility>
#include <vector>

using acecode::tui::y_to_focus;

// 场景:消息列表为空时返回 {-1, 0}。调用方据此 no-op,不应当往
// chat_focus_index 写 -1 之外的任何值。
TEST(ThickVScrollBar, EmptyMessageListReturnsSentinel) {
    auto [idx, off] = y_to_focus(/*mouse_y=*/10, /*track_y_min=*/0,
                                 /*track_height=*/20, /*counts=*/{});
    EXPECT_EQ(idx, -1);
    EXPECT_EQ(off, 0);
}

// 场景:总行数 <= track 高度,意味着所有内容都能装进可见区,根本无需滚动。
// 此时返回 {0, 0} —— 调用方据此把光标定在第一行,等价于一次"跳到顶"。
TEST(ThickVScrollBar, ContentFitsViewportReturnsTop) {
    // 3 + 5 + 2 = 10 行,track 40 行,完全装得下。
    auto [idx, off] = y_to_focus(15, 0, 40, {3, 5, 2});
    EXPECT_EQ(idx, 0);
    EXPECT_EQ(off, 0);
}

// 场景:鼠标在 track 顶部以上(y < track_y_min)被夹紧到 track 顶,
// 等价于点击在 rail 顶端,焦点移到第一行。
TEST(ThickVScrollBar, TopClampGoesToFirstLine) {
    // 30 行 > track 20 行,可滚。
    auto [idx, off] = y_to_focus(/*mouse_y=*/-5, /*track_y_min=*/0,
                                 /*track_height=*/20, {10, 10, 10});
    EXPECT_EQ(idx, 0);
    EXPECT_EQ(off, 0);
}

// 场景:鼠标在 track 底部以下,夹紧到最后一行 —— 这是用户拖到底拽过头的常见操作。
TEST(ThickVScrollBar, BottomClampGoesToLastLine) {
    // 30 行 > track 20 行,可滚。最后一条 10 行,offset 应为 9。
    auto [idx, off] = y_to_focus(/*mouse_y=*/100, /*track_y_min=*/0,
                                 /*track_height=*/20, {10, 10, 10});
    EXPECT_EQ(idx, 2);
    EXPECT_EQ(off, 9);
}

// 场景:鼠标恰好落在 track 中点,期望命中累计行数中点附近。
// 30 总行,中点 ≈ 14,落在第二条消息(10..19)的第 4 行。
TEST(ThickVScrollBar, MidpointHitsMiddleMessage) {
    // track [0, 19],mid_y = 9 或 10。rel = 10,total - 1 = 29,
    // target = 10 * 29 / 19 = 15(整数除法)→ 落在第二条第 5 行。
    auto [idx, off] = y_to_focus(10, 0, 20, {10, 10, 10});
    EXPECT_EQ(idx, 1);
    EXPECT_EQ(off, 5);
}

// 场景:大消息内的精确 line_offset。一条很长的消息(50 行)+ 一条短消息,
// 拖到 track 上方应当落在长消息内部的某一行,而不是直接跳到下一条。
TEST(ThickVScrollBar, InMessageOffsetForTallMessage) {
    // total = 60,track = 20,可滚。track 第 5 行 → target = 5 * 59 / 19 = 15。
    // 第一条 50 行,target 15 落在第一条内部 offset 15。
    auto [idx, off] = y_to_focus(5, 0, 20, {50, 10});
    EXPECT_EQ(idx, 0);
    EXPECT_EQ(off, 15);
}

// 场景:零行消息(theoretically 不应出现,但 message_line_counts 在第一帧前
// 就是 0,我们要保护性地把零当成 1 行处理 —— 与 main.cpp 的 lines_of lambda
// 行为一致)。
TEST(ThickVScrollBar, ZeroLineMessagesTreatedAsOne) {
    // counts {0, 5, 0, 5} → 等价 {1, 5, 1, 5},total = 12,track = 5,可滚。
    // 拖到 track 顶 → target 0 → 第一条(被当 1 行)offset 0。
    auto [idx_top, off_top] = y_to_focus(0, 0, 5, {0, 5, 0, 5});
    EXPECT_EQ(idx_top, 0);
    EXPECT_EQ(off_top, 0);

    // 拖到 track 底 → target 11 → 最后一条(5 行)offset 4。
    auto [idx_bot, off_bot] = y_to_focus(4, 0, 5, {0, 5, 0, 5});
    EXPECT_EQ(idx_bot, 3);
    EXPECT_EQ(off_bot, 4);
}

// 场景:track_y_min 偏移非零(渲染区不从屏幕顶开始)时,映射应当相对偏移正确。
// 这是真实场景:chat_box 上面还有 header,track_y_min 一般是 4-5。
TEST(ThickVScrollBar, NonZeroTrackOriginMapsRelative) {
    // track 从 y=10 到 y=29,height = 20,total = 30,可滚。
    // 鼠标 y=10(track 第 0 行)→ 第一条第 0 行。
    auto [idx_top, off_top] = y_to_focus(10, 10, 20, {10, 10, 10});
    EXPECT_EQ(idx_top, 0);
    EXPECT_EQ(off_top, 0);

    // 鼠标 y=29(track 最后一行)→ 最后一条最后一行。
    auto [idx_bot, off_bot] = y_to_focus(29, 10, 20, {10, 10, 10});
    EXPECT_EQ(idx_bot, 2);
    EXPECT_EQ(off_bot, 9);
}

// 场景:track_height 退化为 1(几乎不该出现,但 reflect 在窗口刚启动时可能
// 给出空 box)。返回 {0, 0} 不崩溃。
TEST(ThickVScrollBar, DegenerateUnitTrackReturnsTop) {
    auto [idx, off] = y_to_focus(0, 0, 1, {10, 10, 10});
    EXPECT_EQ(idx, 0);
    EXPECT_EQ(off, 0);
}
