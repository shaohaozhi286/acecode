#pragma once

#include <algorithm>

namespace acecode::tui {

// /resume picker 视口默认高度。事件处理器与渲染器都引用这个常量,
// 保证 ArrowUp/Down 滚动算法、PgUp/PgDn 跳转步长和 vbox 渲染窗口口径一致。
inline constexpr int kResumePickerVisibleRows = 10;

// /rewind picker(目标列表)视口默认高度。mode 列表条目固定 ≤4 不需要滚动。
inline constexpr int kRewindPickerVisibleRows = 10;

// 斜杠命令 dropdown 视口默认高度。原硬上限 8,放进视口后照常显示 8 行,
// 但可以通过 ArrowUp/Down 把不在窗口内的命令滚动进来。
inline constexpr int kSlashDropdownVisibleRows = 8;

// 把 picker 视口偏移调整到能让选中项保持可见。
// 纯函数,无 FTXUI 依赖,可在 acecode_testable 中直接 include。
// 语义参考 design.md (scrollable-resume-picker):
//   1. visible_rows <= 0 视为 1。
//   2. total <= 0 直接返回 0。
//   3. total <= visible_rows 直接返回 0(全部能装下)。
//   4. clamp prev_offset 到 [0, total - visible_rows]。
//   5. selected < offset 时 offset = selected。
//   6. selected >= offset + visible_rows 时 offset = selected - visible_rows + 1。
//   7. 再 clamp 一次后返回。
inline int scroll_to_keep_visible(int selected, int prev_offset,
                                  int visible_rows, int total) {
    if (total <= 0) return 0;
    if (visible_rows <= 0) visible_rows = 1;
    if (total <= visible_rows) return 0;

    const int max_offset = total - visible_rows;
    int offset = std::clamp(prev_offset, 0, max_offset);

    if (selected < offset) {
        offset = selected;
    } else if (selected >= offset + visible_rows) {
        offset = selected - visible_rows + 1;
    }

    return std::clamp(offset, 0, max_offset);
}

}  // namespace acecode::tui
