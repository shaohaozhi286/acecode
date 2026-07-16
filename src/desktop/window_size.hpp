#pragma once

#include <algorithm>

namespace acecode::desktop {

struct WindowSize {
    int width = 1;
    int height = 1;
};

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

} // namespace acecode::desktop
