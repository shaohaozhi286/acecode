#include "tui/compact_animation.hpp"

#include <gtest/gtest.h>

#include <algorithm>

using acecode::tui::make_compact_animation_frame;

TEST(CompactAnimationFrame, StartsFullyHighlightedThenClearsSymmetricEdges) {
    const auto initial = make_compact_animation_frame(10, 0);
    ASSERT_EQ(initial.highlighted_background.size(), 10u);
    EXPECT_EQ(initial.cleared_from_each_edge, 0u);
    EXPECT_TRUE(std::all_of(initial.highlighted_background.begin(),
                            initial.highlighted_background.end(),
                            [](bool highlighted) { return highlighted; }));

    const auto next = make_compact_animation_frame(10, 100);
    ASSERT_EQ(next.highlighted_background.size(), 10u);
    EXPECT_EQ(next.cleared_from_each_edge, 1u);
    EXPECT_FALSE(next.highlighted_background.front());
    EXPECT_FALSE(next.highlighted_background.back());
    EXPECT_TRUE(next.highlighted_background[1]);
    EXPECT_TRUE(next.highlighted_background[8]);
    for (std::size_t i = 0; i < next.highlighted_background.size(); ++i) {
        EXPECT_EQ(next.highlighted_background[i],
                  next.highlighted_background[
                      next.highlighted_background.size() - 1 - i]);
    }
}

TEST(CompactAnimationFrame, ClearsCenterAndWrapsToFullHighlight) {
    constexpr std::size_t glyph_count = 26;
    const auto center = make_compact_animation_frame(glyph_count, 1300);
    EXPECT_EQ(center.cleared_from_each_edge, 13u);
    EXPECT_TRUE(std::none_of(center.highlighted_background.begin(),
                             center.highlighted_background.end(),
                             [](bool highlighted) { return highlighted; }));

    const auto wrapped = make_compact_animation_frame(glyph_count, 1400);
    EXPECT_EQ(wrapped.cleared_from_each_edge, 0u);
    EXPECT_TRUE(std::all_of(wrapped.highlighted_background.begin(),
                            wrapped.highlighted_background.end(),
                            [](bool highlighted) { return highlighted; }));
}

TEST(CompactAnimationFrame, UsesElapsedTimeAndHandlesBounds) {
    const auto direct = make_compact_animation_frame(9, 750);
    (void)make_compact_animation_frame(9, 20);
    (void)make_compact_animation_frame(9, 60);
    const auto after_skips = make_compact_animation_frame(9, 750);
    EXPECT_EQ(after_skips.cleared_from_each_edge,
              direct.cleared_from_each_edge);
    EXPECT_EQ(after_skips.highlighted_background,
              direct.highlighted_background);

    EXPECT_TRUE(make_compact_animation_frame(0, 1000)
                    .highlighted_background.empty());
    EXPECT_EQ(make_compact_animation_frame(5, -100).highlighted_background,
              make_compact_animation_frame(5, 0).highlighted_background);
}
