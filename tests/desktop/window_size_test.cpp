#include "desktop/window_size.hpp"

#include <gtest/gtest.h>

namespace acecode::desktop {
namespace {

TEST(WindowSizeTest, PreservesRequestedSizeWhenItFits) {
    const auto size = clamp_window_size_to_work_area({1280, 820}, {1920, 1040});

    EXPECT_EQ(size.width, 1280);
    EXPECT_EQ(size.height, 820);
}

TEST(WindowSizeTest, ClampsEachOversizedDimensionIndependently) {
    const auto width_limited = clamp_window_size_to_work_area({1280, 700}, {1024, 740});
    EXPECT_EQ(width_limited.width, 1024);
    EXPECT_EQ(width_limited.height, 700);

    const auto both_limited = clamp_window_size_to_work_area({1280, 820}, {1024, 720});
    EXPECT_EQ(both_limited.width, 1024);
    EXPECT_EQ(both_limited.height, 720);
}

TEST(WindowSizeTest, NormalizesDegenerateInputsToPositiveDimensions) {
    const auto size = clamp_window_size_to_work_area({0, -20}, {-1, 0});

    EXPECT_EQ(size.width, 1);
    EXPECT_EQ(size.height, 1);
}

} // namespace
} // namespace acecode::desktop
