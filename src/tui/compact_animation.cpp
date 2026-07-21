#include "tui/compact_animation.hpp"

#include <algorithm>
#include <cmath>

namespace acecode { namespace tui {

CompactAnimationFrame make_compact_animation_frame(std::size_t glyph_count,
                                                   std::int64_t elapsed_ms) {
    CompactAnimationFrame frame;
    if (glyph_count == 0) return frame;

    elapsed_ms = std::max<std::int64_t>(0, elapsed_ms);
    const std::size_t max_clear = (glyph_count + 1) / 2;
    const double cycle_phases = static_cast<double>(max_clear + 1);
    const double elapsed_phases =
        static_cast<double>(elapsed_ms) *
        kCompactBackgroundPairsPerSecond / 1000.0;
    frame.cleared_from_each_edge = static_cast<std::size_t>(
        std::floor(std::fmod(elapsed_phases, cycle_phases)));

    frame.highlighted_background.reserve(glyph_count);
    for (std::size_t i = 0; i < glyph_count; ++i) {
        frame.highlighted_background.push_back(
            i >= frame.cleared_from_each_edge &&
            i + frame.cleared_from_each_edge < glyph_count);
    }
    return frame;
}

}} // namespace acecode::tui
