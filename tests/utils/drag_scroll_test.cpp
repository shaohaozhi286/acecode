// 覆盖 src/utils/drag_scroll.hpp 的两个纯逻辑组件:
//   1. classify       — 鼠标 y + chat_box 范围 + 左键状态 → Phase 状态机决策
//   2. should_tick    — 按固定节奏 (默认 60ms/行) 放行滚动 tick 的时间门
// 这是拖动选择 + 边界自动滚动功能的纯算法层,跟 FTXUI 完全解耦,
// main.cpp 的鼠标事件处理 + anim_thread 把状态机的输出接到 scroll_chat_by_lines
// 上. 测试不依赖 FTXUI, 保证在 acecode_testable 静态库下能单独构建运行.

#include <gtest/gtest.h>

#include "utils/drag_scroll.hpp"

#include <chrono>

using acecode::drag_scroll::Phase;
using acecode::drag_scroll::Config;
using acecode::drag_scroll::classify;
using acecode::drag_scroll::should_tick;

namespace {
constexpr Config kDefaultCfg{};  // top_edge=1, bot_edge=1, tick_interval=60ms
}  // namespace

// 场景:鼠标落在 chat_box 顶部 1 行内且按住左键时, 应进入 ScrollingUp 触发上滚.
// box 为 [y_min=5, y_max=20], 鼠标 y=5 刚好等于 y_min, 位于 top_edge 内.
TEST(Classify, TopEdgeReturnsScrollingUp) {
    EXPECT_EQ(classify(5, 5, 20, true, kDefaultCfg), Phase::ScrollingUp);
    // y=4 (甚至在 box 之外) 也该触发上滚, 因为用户往上拖出了边界
    EXPECT_EQ(classify(4, 5, 20, true, kDefaultCfg), Phase::ScrollingUp);
}

// 场景:鼠标落在 chat_box 底部 1 行内且按住左键时, 应进入 ScrollingDown 触发下滚.
TEST(Classify, BottomEdgeReturnsScrollingDown) {
    EXPECT_EQ(classify(20, 5, 20, true, kDefaultCfg), Phase::ScrollingDown);
    EXPECT_EQ(classify(25, 5, 20, true, kDefaultCfg), Phase::ScrollingDown);
}

// 场景:左键没按下时, 不论 y 在哪都应保持 Idle — 我们只关心"拖动中"的自动滚动.
// 这避免了普通鼠标悬停在聊天区顶部就意外触发滚动.
TEST(Classify, NotPressedAlwaysIdle) {
    EXPECT_EQ(classify(5, 5, 20, false, kDefaultCfg), Phase::Idle);
    EXPECT_EQ(classify(12, 5, 20, false, kDefaultCfg), Phase::Idle);
    EXPECT_EQ(classify(20, 5, 20, false, kDefaultCfg), Phase::Idle);
}

// 场景:鼠标在 chat_box 中间 (非边界区域), 左键按住 → 应保持 Dragging
// (用户在画选区但还没到触发自动滚动的边界).
TEST(Classify, InteriorReturnsDragging) {
    EXPECT_EQ(classify(10, 5, 20, true, kDefaultCfg), Phase::Dragging);
    EXPECT_EQ(classify(15, 5, 20, true, kDefaultCfg), Phase::Dragging);
}

// 场景:chat_box 尚未被 reflect 填充 (y_max <= y_min) 时, classify 应返回 Idle,
// 不能因为退化的 box 范围误触发滚动. 首帧之前 Renderer 还没跑过, 这是真实会出现的场景.
TEST(Classify, InvalidBoxReturnsIdle) {
    EXPECT_EQ(classify(0, 0, 0, true, kDefaultCfg), Phase::Idle);
    // 反向范围: y_max < y_min
    EXPECT_EQ(classify(10, 20, 5, true, kDefaultCfg), Phase::Idle);
}

// 场景:顶部/底部的 edge 宽度可通过 Config 配置. 把 top_edge 扩大到 3,
// 距 y_min 3 行内都要算 ScrollingUp.
TEST(Classify, ConfigurableEdgeWidth) {
    Config cfg;
    cfg.top_edge_lines = 3;
    cfg.bot_edge_lines = 3;
    // y_min=10, top_edge=3 → y < 13 全部触发上滚
    EXPECT_EQ(classify(12, 10, 30, true, cfg), Phase::ScrollingUp);
    EXPECT_EQ(classify(13, 10, 30, true, cfg), Phase::Dragging);
    // y_max=30, bot_edge=3 → y > 27 全部触发下滚
    EXPECT_EQ(classify(28, 10, 30, true, cfg), Phase::ScrollingDown);
    EXPECT_EQ(classify(27, 10, 30, true, cfg), Phase::Dragging);
}

// 场景:last_tick 零值哨兵 (从未触发过) 必须让第一次调用直接放行, 并写入 now.
// 如果返回 false 就会死锁在"永远等 interval"里.
TEST(ShouldTick, FirstCallAlwaysFires) {
    auto now = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last{};  // 零值
    EXPECT_TRUE(should_tick(now, last, std::chrono::milliseconds(60)));
    // 放行后 last 已被更新为 now
    EXPECT_EQ(last, now);
}

// 场景:两次调用间隔小于 interval 时不应放行;到达或超过 interval 时放行并刷新 last.
// 用手工造的 time_point 模拟时钟, 避免依赖 sleep 带来的时序抖动.
TEST(ShouldTick, RespectsInterval) {
    using namespace std::chrono;
    auto t0 = steady_clock::time_point{} + milliseconds(1000);
    std::chrono::steady_clock::time_point last{};
    // t0 首次放行
    EXPECT_TRUE(should_tick(t0, last, milliseconds(60)));
    EXPECT_EQ(last, t0);

    // 30ms 后不够 60ms 间隔, 不放行
    auto t1 = t0 + milliseconds(30);
    EXPECT_FALSE(should_tick(t1, last, milliseconds(60)));
    EXPECT_EQ(last, t0);  // last 未更新

    // 60ms 后刚好到达, 放行
    auto t2 = t0 + milliseconds(60);
    EXPECT_TRUE(should_tick(t2, last, milliseconds(60)));
    EXPECT_EQ(last, t2);

    // 再往后 10ms (距离上次放行只有 10ms), 不放行
    auto t3 = t2 + milliseconds(10);
    EXPECT_FALSE(should_tick(t3, last, milliseconds(60)));
}

// 场景:一次长时间不调用后, 下次调用应立刻放行 (不会因为差值过大就陷入异常状态).
TEST(ShouldTick, LongGapStillFires) {
    using namespace std::chrono;
    auto t0 = steady_clock::time_point{} + seconds(1);
    std::chrono::steady_clock::time_point last{};
    EXPECT_TRUE(should_tick(t0, last, milliseconds(60)));

    auto t1 = t0 + seconds(10);  // 远大于 interval
    EXPECT_TRUE(should_tick(t1, last, milliseconds(60)));
    EXPECT_EQ(last, t1);
}
