#include "drag_scroll.hpp"

namespace acecode::drag_scroll {

Phase classify(int mouse_y, int box_y_min, int box_y_max,
               bool left_pressed, const Config& cfg) {
    if (!left_pressed) {
        return Phase::Idle;
    }
    // box 尚未填充(reflect 在首帧前) → 不进入任何滚动阶段
    if (box_y_max <= box_y_min) {
        return Phase::Idle;
    }
    if (mouse_y < box_y_min + cfg.top_edge_lines) {
        return Phase::ScrollingUp;
    }
    if (mouse_y > box_y_max - cfg.bot_edge_lines) {
        return Phase::ScrollingDown;
    }
    return Phase::Dragging;
}

bool should_tick(std::chrono::steady_clock::time_point now,
                 std::chrono::steady_clock::time_point& last_tick,
                 std::chrono::milliseconds interval) {
    // 零值哨兵:从未触发过,直接放行并记录第一次时间戳
    if (last_tick.time_since_epoch().count() == 0) {
        last_tick = now;
        return true;
    }
    if (now - last_tick >= interval) {
        last_tick = now;
        return true;
    }
    return false;
}

}  // namespace acecode::drag_scroll
