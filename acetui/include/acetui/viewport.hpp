// acetui/viewport.hpp — widget 渲染区域的矩形描述。
//
// 所有坐标 1-based(与 ANSI/VT cursor escape 序列一致),left-top origin。
// "贴底" 构造对应 codex 的 "viewport 占据屏幕底部 N 行" 模式 ——
// scrollback 在 viewport 之上,widget 在 viewport 之内。

#pragma once

#include "acetui/terminal.hpp"

namespace acetui {

struct Viewport {
    int top    = 1;
    int left   = 1;
    int width  = 0;
    int height = 0;

    // viewport 最后一行的行号(1-based)。
    int bottom_row() const { return top + height - 1; }

    // 给定屏幕尺寸 + 期望 widget 高度,返回贴底放置的 viewport。
    // top 被 clamp 到 ≥ 1 以应对屏幕过小的情形。
    static Viewport bottom(Size screen, int height);
};

}  // namespace acetui
