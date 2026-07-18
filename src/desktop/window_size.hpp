#pragma once

#include <algorithm>
#include <limits>

namespace acecode::desktop {

struct WindowSize {
    int width = 1;
    int height = 1;
};

inline constexpr int kDesktopWindowSafeHorizontalMarginDip = 40;
inline constexpr int kDesktopWindowSafeVerticalMarginDip = 60;

// Clamp an outer native window size to a positive usable display area.
// Degenerate inputs are normalized to one pixel so callers never hand an
// invalid zero/negative dimension to a platform window API.
inline WindowSize clamp_window_size_to_work_area(WindowSize requested,
                                                  WindowSize work_area) {
    const int max_width = std::max(1, work_area.width);
    const int max_height = std::max(1, work_area.height);
    return {
        std::min(std::max(1, requested.width), max_width),
        std::min(std::max(1, requested.height), max_height),
    };
}

inline int window_dip_to_pixels(int value, int dpi) {
    if (value <= 0) return 0;
    const int effective_dpi = dpi > 0 ? dpi : 96;
    const long long scaled =
        (static_cast<long long>(value) * static_cast<long long>(effective_dpi)) / 96;
    return static_cast<int>(std::min<long long>(
        std::numeric_limits<int>::max(), std::max<long long>(0, scaled)));
}

// Fit the preferred initial outer-window size inside the usable display area
// while preserving visible breathing room around the first screen.
inline WindowSize fit_desktop_window_to_safe_work_area(WindowSize requested,
                                                        WindowSize work_area,
                                                        int dpi) {
    const int horizontal_margin =
        window_dip_to_pixels(kDesktopWindowSafeHorizontalMarginDip, dpi);
    const int vertical_margin =
        window_dip_to_pixels(kDesktopWindowSafeVerticalMarginDip, dpi);
    const long long safe_width = std::max<long long>(
        1,
        static_cast<long long>(work_area.width) -
            2LL * static_cast<long long>(horizontal_margin));
    const long long safe_height = std::max<long long>(
        1,
        static_cast<long long>(work_area.height) -
            2LL * static_cast<long long>(vertical_margin));
    return clamp_window_size_to_work_area(
        requested,
        {static_cast<int>(safe_width), static_cast<int>(safe_height)});
}

} // namespace acecode::desktop
