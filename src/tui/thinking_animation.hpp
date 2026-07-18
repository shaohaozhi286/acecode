#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace acecode { namespace tui {

inline constexpr int kThinkingAnimationFrameMs = 60;
inline constexpr int kDefaultAnimationFrameMs = 300;
inline constexpr int kConhostAnimationFrameMs = 1000;
inline constexpr int kDragAutoscrollFrameMs = 50;

inline constexpr double kThinkingShimmerCellsPerSecond = 6.75;
inline constexpr double kThinkingShimmerEdgePaddingCells = 2.5;

struct ThinkingGlyphHighlight {
    float warm = 0.0f;
    float white = 0.0f;
};

struct ThinkingAnimationFrame {
    double highlight_center = -kThinkingShimmerEdgePaddingCells;
    std::vector<ThinkingGlyphHighlight> glyph_highlights;
};

// Select the shared ticker's next wake interval. Drag autoscroll keeps the
// highest priority; the faster thinking cadence is used only while the modern
// thinking row is actually visible.
int select_animation_frame_interval_ms(bool conhost_compat_layout,
                                       bool thinking_visible,
                                       bool drag_autoscroll_active);

// Build one deterministic shimmer frame. Each glyph receives bounded warm
// trail and white-core weights for two-stage color interpolation.
ThinkingAnimationFrame make_thinking_animation_frame(std::size_t glyph_count,
                                                      std::int64_t elapsed_ms);

}} // namespace acecode::tui
