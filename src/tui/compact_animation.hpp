#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace acecode { namespace tui {

inline constexpr double kCompactBackgroundPairsPerSecond = 10.0;

struct CompactAnimationFrame {
    std::size_t cleared_from_each_edge = 0;
    std::vector<bool> highlighted_background;
};

// Build a deterministic cell-aligned mask for the compacting row. A cycle
// begins fully highlighted, then clears matching glyphs from both edges until
// no highlighted center remains before wrapping to the initial frame.
CompactAnimationFrame make_compact_animation_frame(std::size_t glyph_count,
                                                   std::int64_t elapsed_ms);

}} // namespace acecode::tui
