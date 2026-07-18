#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace acecode { namespace tui {

inline constexpr int kThinkingAnimationFrameMs = 80;
inline constexpr int kDefaultAnimationFrameMs = 300;
inline constexpr int kConhostAnimationFrameMs = 1000;
inline constexpr int kDragAutoscrollFrameMs = 50;

inline constexpr double kThinkingShimmerCellsPerSecond = 4.5;
inline constexpr double kThinkingShimmerEdgePaddingCells = 2.5;

struct ThinkingAnimationFrame {
    double highlight_center = -kThinkingShimmerEdgePaddingCells;
    std::vector<float> glyph_highlights;
};

// Select the shared ticker's next wake interval. Drag autoscroll keeps the
// highest priority; the faster thinking cadence is used only while the modern
// thinking row is actually visible.
int select_animation_frame_interval_ms(bool conhost_compat_layout,
                                       bool thinking_visible,
                                       bool drag_autoscroll_active);

// Build one deterministic shimmer frame. Each glyph highlight is in [0, 1]
// and can be passed directly to ftxui::Color::Interpolate by the renderer.
ThinkingAnimationFrame make_thinking_animation_frame(std::size_t glyph_count,
                                                      std::int64_t elapsed_ms);

}} // namespace acecode::tui
