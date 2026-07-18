#include "tui/thinking_animation.hpp"

#include <algorithm>
#include <cmath>

namespace acecode { namespace tui {

namespace {

constexpr double kShimmerSigmaCells = 1.0;

} // namespace

int select_animation_frame_interval_ms(bool conhost_compat_layout,
                                       bool thinking_visible,
                                       bool drag_autoscroll_active) {
    if (drag_autoscroll_active) return kDragAutoscrollFrameMs;
    if (conhost_compat_layout) return kConhostAnimationFrameMs;
    if (thinking_visible) return kThinkingAnimationFrameMs;
    return kDefaultAnimationFrameMs;
}

ThinkingAnimationFrame make_thinking_animation_frame(std::size_t glyph_count,
                                                      std::int64_t elapsed_ms) {
    ThinkingAnimationFrame frame;
    if (glyph_count == 0) return frame;

    elapsed_ms = std::max<std::int64_t>(0, elapsed_ms);
    const double last_glyph = static_cast<double>(glyph_count - 1);
    const double cycle_cells =
        last_glyph + 2.0 * kThinkingShimmerEdgePaddingCells;
    const double travelled_cells = std::fmod(
        static_cast<double>(elapsed_ms) *
            kThinkingShimmerCellsPerSecond / 1000.0,
        cycle_cells);
    frame.highlight_center =
        travelled_cells - kThinkingShimmerEdgePaddingCells;

    frame.glyph_highlights.reserve(glyph_count);
    for (std::size_t i = 0; i < glyph_count; ++i) {
        const double distance = static_cast<double>(i) - frame.highlight_center;
        const double normalized = distance / kShimmerSigmaCells;
        const double highlight = std::exp(-0.5 * normalized * normalized);
        frame.glyph_highlights.push_back(
            static_cast<float>(std::clamp(highlight, 0.0, 1.0)));
    }
    return frame;
}

}} // namespace acecode::tui
