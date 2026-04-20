#pragma once

// 拖动选择 + 边界自动滚动的纯逻辑层。
// 状态机和时间门是无副作用的纯函数,与 FTXUI 解耦,可在 acecode_testable 下单测。
// 调用方(main.cpp 的鼠标事件 + anim_thread)负责把状态写回 TuiState、
// 触发实际滚动、补偿 selection 屏幕坐标。

#include <chrono>

namespace acecode::drag_scroll {

enum class Phase {
    Idle,
    Dragging,
    ScrollingUp,
    ScrollingDown,
};

struct Config {
    int top_edge_lines = 1;                              // 距 chat_box 顶部 N 行内触发上滚
    int bot_edge_lines = 1;                              // 距 chat_box 底部 N 行内触发下滚
    std::chrono::milliseconds tick_interval{60};         // 每 60ms 滚 1 行 (~16 行/秒)
};

// 给定鼠标 y、chat_box 行范围、左键是否按住、配置,返回当前应处的阶段。
// 不读不写任何状态。box 无效(y_max <= y_min) 或左键未按下时返回 Idle。
Phase classify(int mouse_y, int box_y_min, int box_y_max,
               bool left_pressed, const Config& cfg);

// 时间门:若 now - last_tick >= interval 则返回 true 并把 last_tick 更新为 now,
// 否则返回 false。last_tick 初始零值视为"从未触发过",首次必触发。
bool should_tick(std::chrono::steady_clock::time_point now,
                 std::chrono::steady_clock::time_point& last_tick,
                 std::chrono::milliseconds interval);

}  // namespace acecode::drag_scroll
