#include "window_chrome.hpp"

#include <algorithm>

namespace acecode::desktop {

int frameless_resize_border(int frame, int padding) {
    return std::max(1, frame + padding);
}

FramelessHitTestArea classify_frameless_hit_test(const FramelessHitTestInput& input) {
    if (input.width <= 0 || input.height <= 0) return FramelessHitTestArea::Client;

    const int border_x = frameless_resize_border(input.frame_x, input.padding);
    const int border_y = frameless_resize_border(input.frame_y, input.padding);
    const int drag_height = std::max(1, input.drag_height);

    if (!input.maximized) {
        const bool left = input.x >= 0 && input.x < border_x;
        const bool right = input.x < input.width && input.x >= input.width - border_x;
        const bool top = input.y >= 0 && input.y < border_y;
        const bool bottom = input.y < input.height && input.y >= input.height - border_y;

        if (top && left) return FramelessHitTestArea::TopLeft;
        if (top && right) return FramelessHitTestArea::TopRight;
        if (bottom && left) return FramelessHitTestArea::BottomLeft;
        if (bottom && right) return FramelessHitTestArea::BottomRight;
        if (top) return FramelessHitTestArea::Top;
        if (bottom) return FramelessHitTestArea::Bottom;
        if (left) return FramelessHitTestArea::Left;
        if (right) return FramelessHitTestArea::Right;
    }

    if (input.y >= 0 && input.y < drag_height) return FramelessHitTestArea::Caption;
    return FramelessHitTestArea::Client;
}

std::optional<FramelessHitTestArea> parse_resize_direction(std::string_view direction) {
    if (direction == "top") return FramelessHitTestArea::Top;
    if (direction == "bottom") return FramelessHitTestArea::Bottom;
    if (direction == "left") return FramelessHitTestArea::Left;
    if (direction == "right") return FramelessHitTestArea::Right;
    if (direction == "top-left") return FramelessHitTestArea::TopLeft;
    if (direction == "top-right") return FramelessHitTestArea::TopRight;
    if (direction == "bottom-left") return FramelessHitTestArea::BottomLeft;
    if (direction == "bottom-right") return FramelessHitTestArea::BottomRight;
    return std::nullopt;
}

} // namespace acecode::desktop