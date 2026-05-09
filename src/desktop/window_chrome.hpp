#pragma once

#include <optional>
#include <string_view>

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

// 把 JS 端传的 direction 字符串映射到 resize 边/角枚举。
// 接受 "top"/"bottom"/"left"/"right" 四边 + "top-left"/"top-right"/
// "bottom-left"/"bottom-right" 四角。其它(包括 "client"/"caption"/空串/
// 大小写不匹配)返回 nullopt — caller 应当拒绝调用,不要默默 fallback,
// 否则前端打错字时会出现"以为在 resize 顶,实际却开始拖动"的诡异行为。
std::optional<FramelessHitTestArea> parse_resize_direction(std::string_view direction);

} // namespace acecode::desktop