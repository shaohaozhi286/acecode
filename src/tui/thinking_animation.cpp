#include "tui/thinking_animation.hpp"

#include <algorithm>
#include <cmath>

namespace acecode { namespace tui {

namespace {

constexpr double kWarmTrailOffsetCells = 1.4;
constexpr double kWarmTrailSigmaCells = 1.1;
constexpr double kWhiteCoreSigmaCells = 0.6;
constexpr double kOffTextFadeSigmaCells = 1.0;

double gaussian_weight(double distance, double sigma) {
    const double normalized = distance / sigma;
    return std::exp(-0.5 * normalized * normalized);
}

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

    double off_text_distance = 0.0;
    if (frame.highlight_center < 0.0) {
        off_text_distance = -frame.highlight_center;
    } else if (frame.highlight_center > last_glyph) {
        off_text_distance = frame.highlight_center - last_glyph;
    }
    const double edge_visibility =
        gaussian_weight(off_text_distance, kOffTextFadeSigmaCells);

    frame.glyph_highlights.reserve(glyph_count);
    for (std::size_t i = 0; i < glyph_count; ++i) {
        const double glyph_position = static_cast<double>(i);
        const double warm_center =
            frame.highlight_center - kWarmTrailOffsetCells;
        const double warm = edge_visibility * gaussian_weight(
            glyph_position - warm_center, kWarmTrailSigmaCells);
        const double white = edge_visibility * gaussian_weight(
            glyph_position - frame.highlight_center, kWhiteCoreSigmaCells);
        frame.glyph_highlights.push_back({
            static_cast<float>(std::clamp(warm, 0.0, 1.0)),
            static_cast<float>(std::clamp(white, 0.0, 1.0)),
        });
    }
    return frame;
}

}} // namespace acecode::tui
