// 覆盖 src/tui/picker_scroll.hpp 中 acecode::tui::scroll_to_keep_visible 的纯函数契约。
// 这是 /resume picker 视口滚动的核心算法:给定当前选中索引、上一帧 offset、
// 视口高度、总条目数,返回新的 offset,使选中项始终位于 [offset, offset+visible_rows) 区间。
// 渲染层和键位事件层都要调它,任何边界处理出错(空列表、视口大于总数、prev_offset
// 非法值)都会让 picker 显示为空白或选中项不可见 —— 对 UX 是直接观感问题,但纯函数本身
// 可以脱离 FTXUI 单独验证,这里就只测纯映射,不测 ftxui 渲染。

#include <gtest/gtest.h>

#include "tui/picker_scroll.hpp"

using acecode::tui::scroll_to_keep_visible;

// 场景:总条目数为 0(picker 数据源为空)时,offset 必须固定为 0,
// 不论 selected/prev_offset 传什么。调用方据此 no-op,不产生越界访问。
TEST(PickerScroll, EmptyTotalReturnsZero) {
    EXPECT_EQ(scroll_to_keep_visible(/*selected=*/0, /*prev_offset=*/0,
                                     /*visible_rows=*/10, /*total=*/0),
              0);
    EXPECT_EQ(scroll_to_keep_visible(5, 3, 10, 0), 0);
    EXPECT_EQ(scroll_to_keep_visible(-1, -7, 10, 0), 0);
}

// 场景:总条目能完全装进可见窗口(total <= visible_rows),不需要任何滚动,
// offset 固定为 0。这是最常见的小项目场景(只有 3 条会话,视口 10 行)。
TEST(PickerScroll, AllItemsFitVisibleWindow) {
    EXPECT_EQ(scroll_to_keep_visible(0, 0, 10, 3), 0);
    EXPECT_EQ(scroll_to_keep_visible(2, 0, 10, 3), 0);
    EXPECT_EQ(scroll_to_keep_visible(2, 5, 10, 3), 0);  // prev_offset 非法也被压回
    EXPECT_EQ(scroll_to_keep_visible(0, 0, 10, 10), 0);  // 边界:刚好等于
}

// 场景:ArrowDown 越下界 — 用户从第 9 项按 Down 到第 10 项,offset 必须从 0
// 滚到 1,使第 10 项成为窗口最末行。这是用户最常体验到的"超出省略"修复点。
TEST(PickerScroll, ArrowDownCrossingLowerEdgeScrolls) {
    // visible=10, total=30, prev_offset=0, selected 从 9 → 10。
    EXPECT_EQ(scroll_to_keep_visible(/*selected=*/10, /*prev_offset=*/0,
                                     /*visible_rows=*/10, /*total=*/30),
              1);
    // 再下一步:selected=11,offset 应跟到 2。
    EXPECT_EQ(scroll_to_keep_visible(11, 1, 10, 30), 2);
}

// 场景:ArrowUp 越上界 — 用户已经把窗口滚到中部(offset=5),
// 持续向上按到选中项小于 offset 时,offset 必须收缩成 selected 自身,
// 让选中项成为窗口最顶行。
TEST(PickerScroll, ArrowUpCrossingUpperEdgeScrolls) {
    // visible=10, total=30, prev_offset=5。
    // selected=4 触发上越:offset 应变为 4。
    EXPECT_EQ(scroll_to_keep_visible(4, 5, 10, 30), 4);
    // selected=2 时再次收缩:offset = 2。
    EXPECT_EQ(scroll_to_keep_visible(2, 5, 10, 30), 2);
}

// 场景:Home 按键 — 用户按 Home 后 selected=0,无论先前 offset 是多少
// 都应得到 offset=0。
TEST(PickerScroll, HomeReturnsZeroOffset) {
    EXPECT_EQ(scroll_to_keep_visible(0, 18, 10, 30), 0);
    EXPECT_EQ(scroll_to_keep_visible(0, 5, 10, 30), 0);
    EXPECT_EQ(scroll_to_keep_visible(0, 0, 10, 30), 0);
}

// 场景:End 按键 — 用户按 End 后 selected=total-1,offset 应停在
// total - visible_rows(让最后一页正好可见)。这是 PgDn/End 收尾时的关键不变量。
TEST(PickerScroll, EndAlignsLastPage) {
    // visible=10, total=30, selected=29 → offset 应为 20。
    EXPECT_EQ(scroll_to_keep_visible(29, 0, 10, 30), 20);
    EXPECT_EQ(scroll_to_keep_visible(29, 5, 10, 30), 20);
    EXPECT_EQ(scroll_to_keep_visible(29, 25, 10, 30), 20);
}

// 场景:PgDn 接近末尾 — 用户在 selected=24 处按 PgDn,加 visible_rows 后
// 跳到 selected=34,但 total=30,事件层会先 clamp selected=29。
// 这里直接断言 clamp 后调用算法的结果不越界。
TEST(PickerScroll, PgDnNearEndDoesNotOvershoot) {
    // selected=29(已 clamp 到 last),prev_offset=15。
    EXPECT_EQ(scroll_to_keep_visible(29, 15, 10, 30), 20);
    // 模拟连续 PgDn 直到末尾:每次 selected 推到末尾,offset 应稳定在 20。
    EXPECT_EQ(scroll_to_keep_visible(29, 20, 10, 30), 20);
}

// 场景:prev_offset 已经超过合法范围(例如 total 缩小后窗口未刷新),
// 算法必须先 clamp prev_offset 再计算。否则可能输出非法 offset。
TEST(PickerScroll, ClampsIllegalPrevOffset) {
    // total=15, visible=10,合法 offset 范围 [0, 5]。
    // prev_offset=99,selected=0 → offset 应为 0(因 selected 在合法 offset 之上)。
    EXPECT_EQ(scroll_to_keep_visible(0, 99, 10, 15), 0);
    // prev_offset=-7,selected=14(末尾) → offset 应为 5。
    EXPECT_EQ(scroll_to_keep_visible(14, -7, 10, 15), 5);
    // prev_offset=99,selected=12,合法 → offset 必在 [0,5];12 在窗口 [3,12]
    // 区间内即可,clamp 后选 5(让 selected 落在最后一页底部)。
    EXPECT_EQ(scroll_to_keep_visible(12, 99, 10, 15), 5);
}

// 场景:visible_rows 退化为 0 或负数(理论不该出现,但防御性测试),
// 算法应把它视为 1,不崩溃也不返回非法值。
TEST(PickerScroll, DegenerateVisibleRowsFallbackToOne) {
    // total=5, visible 当 1,selected=2,offset 应为 2(selected 自身)。
    EXPECT_EQ(scroll_to_keep_visible(2, 0, 0, 5), 2);
    EXPECT_EQ(scroll_to_keep_visible(2, 0, -3, 5), 2);
    // selected=4 → offset=4。
    EXPECT_EQ(scroll_to_keep_visible(4, 0, 0, 5), 4);
}

// 场景:selected 已经在可见窗口内,offset 不应被无谓地改动。
// 这是稳定性保证:用户在窗口内连续 ArrowDown 不应该让窗口"反复跳动"。
TEST(PickerScroll, SelectedAlreadyVisibleKeepsOffset) {
    // visible=10, total=30, prev_offset=5。selected 在 [5,14] 内随便挑。
    EXPECT_EQ(scroll_to_keep_visible(5, 5, 10, 30), 5);
    EXPECT_EQ(scroll_to_keep_visible(10, 5, 10, 30), 5);
    EXPECT_EQ(scroll_to_keep_visible(14, 5, 10, 30), 5);
}
