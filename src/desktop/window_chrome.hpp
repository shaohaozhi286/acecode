#pragma once

namespace acecode::desktop {

enum class FramelessHitTestArea {
    Client,
    Caption,
    Left,
    Right,
    Top,
    TopLeft,
    TopRight,
    Bottom,
    BottomLeft,
    BottomRight,
};

struct FramelessHitTestInput {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int frame_x = 0;
    int frame_y = 0;
    int padding = 0;
    int drag_height = 0;
    bool maximized = false;
};

int frameless_resize_border(int frame, int padding);
FramelessHitTestArea classify_frameless_hit_test(const FramelessHitTestInput& input);

} // namespace acecode::desktop