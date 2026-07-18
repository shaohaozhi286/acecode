// TUI 等待态流光动画的纯模型单测。
// 变更来源:openspec/changes/smooth-tui-thinking-animation —— 将原来每
// 300ms 跳一个整字符的离散动画改为基于真实经过时间的连续亮度插值。

#include "tui/thinking_animation.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

using acecode::tui::kConhostAnimationFrameMs;
using acecode::tui::kDefaultAnimationFrameMs;
using acecode::tui::kDragAutoscrollFrameMs;
using acecode::tui::kThinkingAnimationFrameMs;
using acecode::tui::kThinkingShimmerCellsPerSecond;
using acecode::tui::kThinkingShimmerEdgePaddingCells;
using acecode::tui::make_thinking_animation_frame;
using acecode::tui::select_animation_frame_interval_ms;

// 触发场景:共享 ticker 在拖动、旧终端、现代 thinking 与空闲状态间选择节奏。
// 期望行为:拖动优先级最高;旧终端保持 1s;只有可见 thinking 使用 60ms。
TEST(ThinkingAnimationCadence, SelectsActiveOnlyIntervalWithStablePriority) {
    EXPECT_EQ(select_animation_frame_interval_ms(false, true, true),
              kDragAutoscrollFrameMs);
    EXPECT_EQ(select_animation_frame_interval_ms(true, true, false),
              kConhostAnimationFrameMs);
    EXPECT_EQ(select_animation_frame_interval_ms(false, true, false),
              kThinkingAnimationFrameMs);
    EXPECT_EQ(select_animation_frame_interval_ms(false, false, false),
              kDefaultAnimationFrameMs);

    EXPECT_EQ(kDragAutoscrollFrameMs, 50);
    EXPECT_EQ(kThinkingAnimationFrameMs, 60);
    EXPECT_EQ(kDefaultAnimationFrameMs, 300);
    EXPECT_EQ(kConhostAnimationFrameMs, 1000);
}

// 触发场景:为短语和固定三个点生成一帧双色流光权重。
// 期望行为:每个 glyph 都有一组可用于两段颜色插值的 [0,1] 权重。
TEST(ThinkingAnimationFrame, ProducesBoundedWarmAndWhiteWeightsPerGlyph) {
    const auto frame = make_thinking_animation_frame(9, 640);
    ASSERT_EQ(frame.glyph_highlights.size(), 9u);
    float peak_warm = 0.0f;
    float peak_white = 0.0f;
    for (const auto& highlight : frame.glyph_highlights) {
        EXPECT_GE(highlight.warm, 0.0f);
        EXPECT_LE(highlight.warm, 1.0f);
        EXPECT_GE(highlight.white, 0.0f);
        EXPECT_LE(highlight.white, 1.0f);
        peak_warm = std::max(peak_warm, highlight.warm);
        peak_white = std::max(peak_white, highlight.white);
    }
    EXPECT_GT(peak_warm, 0.8f);
    EXPECT_GT(peak_white, 0.8f);
}

// 触发场景:相邻两帧相隔产品设定的 60ms。
// 期望行为:速度从 4.5 精确增加 50% 到 6.75 cells/s,高光中心每帧
// 只移动 0.405 个 cell,多个 glyph 的中间亮度随之渐变,
// 而不是直接从一个整字符跳到下一个整字符。
TEST(ThinkingAnimationFrame, AdjacentFramesInterpolateSubcellMovement) {
    EXPECT_DOUBLE_EQ(kThinkingShimmerCellsPerSecond, 4.5 * 1.5);

    const auto first = make_thinking_animation_frame(12, 500);
    const auto next = make_thinking_animation_frame(
        12, 500 + kThinkingAnimationFrameMs);

    const double expected_delta =
        kThinkingShimmerCellsPerSecond * kThinkingAnimationFrameMs / 1000.0;
    EXPECT_NEAR(next.highlight_center - first.highlight_center,
                expected_delta, 1e-9);

    int meaningfully_changed = 0;
    int intermediate_weights = 0;
    for (std::size_t i = 0; i < first.glyph_highlights.size(); ++i) {
        const auto& first_highlight = first.glyph_highlights[i];
        const auto& next_highlight = next.glyph_highlights[i];
        if (std::abs(next_highlight.warm - first_highlight.warm) > 0.02f ||
            std::abs(next_highlight.white - first_highlight.white) > 0.02f) {
            ++meaningfully_changed;
        }
        if ((next_highlight.warm > 0.05f &&
             next_highlight.warm < 0.95f) ||
            (next_highlight.white > 0.05f &&
             next_highlight.white < 0.95f)) {
            ++intermediate_weights;
        }
    }
    EXPECT_GE(meaningfully_changed, 2);
    EXPECT_GE(intermediate_weights, 2);
}

// 触发场景:流光核心落在一个整 glyph 上,观察移动方向前后的权重。
// 期望行为:左侧尾迹以黄色为主,核心达到亮白,右侧前沿快速回落到灰色。
TEST(ThinkingAnimationFrame, OrdersWarmTrailWhiteCoreAndGrayLeadingEdge) {
    const auto frame = make_thinking_animation_frame(9, 963);
    ASSERT_EQ(frame.glyph_highlights.size(), 9u);
    EXPECT_NEAR(frame.highlight_center, 4.0, 0.001);

    const auto& warm_trail = frame.glyph_highlights[3];
    const auto& white_core = frame.glyph_highlights[4];
    const auto& gray_leading_edge = frame.glyph_highlights[6];

    EXPECT_GT(warm_trail.warm, 0.9f);
    EXPECT_LT(warm_trail.white, 0.3f);
    EXPECT_GT(white_core.white, 0.99f);
    EXPECT_GT(white_core.white, white_core.warm);
    EXPECT_LT(gray_leading_edge.warm, 0.02f);
    EXPECT_LT(gray_leading_edge.white, 0.02f);
}

// 触发场景:中间若干 redraw 被调度器跳过,随后直接请求当前时间的一帧。
// 期望行为:结果只由 elapsed_ms 决定,与之前生成过多少帧完全无关。
TEST(ThinkingAnimationFrame, ElapsedTimestampIsDeterministicAcrossSkippedFrames) {
    const auto direct = make_thinking_animation_frame(10, 1370);
    (void)make_thinking_animation_frame(10, 80);
    (void)make_thinking_animation_frame(10, 160);
    const auto after_skips = make_thinking_animation_frame(10, 1370);

    EXPECT_DOUBLE_EQ(after_skips.highlight_center, direct.highlight_center);
    ASSERT_EQ(after_skips.glyph_highlights.size(),
              direct.glyph_highlights.size());
    for (std::size_t i = 0; i < direct.glyph_highlights.size(); ++i) {
        EXPECT_FLOAT_EQ(after_skips.glyph_highlights[i].warm,
                        direct.glyph_highlights[i].warm);
        EXPECT_FLOAT_EQ(after_skips.glyph_highlights[i].white,
                        direct.glyph_highlights[i].white);
    }
}

// 触发场景:流光从末尾绕回开头。
// 期望行为:中心在可见文本两侧各留 2.5 cell 的淡出区,额外的边缘
// 可见度包络也会压低较长的黄色尾迹,不会从末字符硬切到首字符。
TEST(ThinkingAnimationFrame, WrapsThroughOffTextFadePadding) {
    constexpr std::size_t glyph_count = 9;
    const double cycle_cells = static_cast<double>(glyph_count - 1) +
        2.0 * kThinkingShimmerEdgePaddingCells;
    const double cycle_ms =
        cycle_cells * 1000.0 / kThinkingShimmerCellsPerSecond;

    const auto before_wrap = make_thinking_animation_frame(
        glyph_count, static_cast<long long>(std::floor(cycle_ms)));
    const auto after_wrap = make_thinking_animation_frame(
        glyph_count, static_cast<long long>(std::ceil(cycle_ms)));

    float before_peak = 0.0f;
    float after_peak = 0.0f;
    for (const auto& highlight : before_wrap.glyph_highlights) {
        before_peak = std::max({before_peak, highlight.warm, highlight.white});
    }
    for (const auto& highlight : after_wrap.glyph_highlights) {
        after_peak = std::max({after_peak, highlight.warm, highlight.white});
    }
    EXPECT_LT(before_peak, 0.03f);
    EXPECT_LT(after_peak, 0.03f);
}

// 触发场景:空短语或防御性负 elapsed 输入。
// 期望行为:空输入安全返回;负时间按动画起点处理,不产生反向 phase。
TEST(ThinkingAnimationFrame, HandlesEmptyAndNegativeInputs) {
    const auto empty = make_thinking_animation_frame(0, 1000);
    EXPECT_TRUE(empty.glyph_highlights.empty());

    const auto negative = make_thinking_animation_frame(5, -100);
    const auto zero = make_thinking_animation_frame(5, 0);
    EXPECT_DOUBLE_EQ(negative.highlight_center, zero.highlight_center);
    ASSERT_EQ(negative.glyph_highlights.size(), zero.glyph_highlights.size());
    for (std::size_t i = 0; i < zero.glyph_highlights.size(); ++i) {
        EXPECT_FLOAT_EQ(negative.glyph_highlights[i].warm,
                        zero.glyph_highlights[i].warm);
        EXPECT_FLOAT_EQ(negative.glyph_highlights[i].white,
                        zero.glyph_highlights[i].white);
    }
}
