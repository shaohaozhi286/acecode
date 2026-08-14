#!/usr/bin/env python3
"""04 - RGBA alpha 透明混色对比

报告章节:3.1 / 3.4
acecode 现状:❌ FTXUI 颜色不透明(OPAQUE),无法做半透明叠层
有此能力:opencode(RGBA alpha 混色,半透明对话框遮罩、透出终端的 system 主题)、
          grok-build(framebuffer 合成)

ANSI 本身无法逐 cell 设 alpha。opencode/grok 在 framebuffer 层做 per-pixel alpha 合成:
  result = bg * (1 - alpha) + overlay * alpha
先把叠层与背景混色,再输出最终颜色。本演示用预混色模拟这个视觉效果。

演示:
  左:acecode 现状 -- 不透明遮罩完全盖住背景文字
  右:opencode/grok -- 半透明遮罩,背景文字隐约可见(预混色模拟)
"""

import sys
import time
from _term import init, RESET, BOLD, DIM, fg, bg, goto, CLEAR, HIDE_CURSOR, SHOW_CURSOR, blend

init()


def render_row(row, label, alpha):
    """在第 row 行演示一个遮罩盖住背景文字的效果。

    acecode(不透明):遮罩色直接盖死背景。
    opencode(半透明):遮罩色与背景文字色按 alpha 预混,文字隐约可见。
    """
    bg_text = "这是一段背景文字 transcript content streaming here..."
    bg_color = (60, 60, 70)      # 深灰背景文字
    panel_color = (40, 120, 200) # 蓝色对话框遮罩

    # 标签
    sys.stdout.write(goto(row, 1) + BOLD + f"{label:<28}" + RESET)

    col = 30
    # 背景文字(裸色)
    sys.stdout.write(goto(row, col))
    for ch in bg_text[:40]:
        sys.stdout.write(bg(30, 30, 38) + fg(*bg_color) + ch)
    sys.stdout.write(RESET)

    # 遮罩盖在前 20 个字符上
    if alpha >= 1.0:
        # acecode: 不透明,完全盖死
        sys.stdout.write(goto(row, col))
        for ch in bg_text[:20]:
            sys.stdout.write(bg(*panel_color) + " " )
        sys.stdout.write(RESET + fg(255, 255, 255) + BOLD)
        sys.stdout.write(goto(row, col + 2) + "[遮罩完全盖住]")
    else:
        # opencode: 半透明,背景文字与遮罩预混
        sys.stdout.write(goto(row, col))
        mixed_bg = blend((30, 30, 38), panel_color, alpha)
        mixed_fg = blend(bg_color, (255, 255, 255), alpha)
        for ch in bg_text[:20]:
            sys.stdout.write(bg(*mixed_bg) + fg(*mixed_fg) + ch)
        sys.stdout.write(RESET + fg(255, 255, 255) + BOLD)
        sys.stdout.write(goto(row, col + 2) + "[半透明]")
    sys.stdout.write(RESET)


def main():
    sys.stdout.write(CLEAR + HIDE_CURSOR)
    try:
        sys.stdout.write(goto(1, 1) + BOLD +
                         "RGBA alpha 透明混色对比(opencode/grok 有,acecode 无)" + RESET)
        sys.stdout.write(goto(2, 1) + DIM +
                         "ANSI 无法逐 cell 设 alpha;opencode/grok 在 framebuffer 层 per-pixel 合成。"
                         "这里用预混色模拟视觉效果。" + RESET)

        # 三个 alpha 档位渐变
        labels = [
            (5, "acecode(不透明 α=1.0)", 1.0),
            (8, "opencode(半透明 α=0.5)", 0.5),
            (11, "opencode(半透明 α=0.3)", 0.3),
        ]
        for row, label, a in labels:
            render_row(row, label, a)

        # 动画:alpha 从 0.2 渐变到 0.8 再回来,展示"呼吸"的透明度
        sys.stdout.write(goto(14, 1) + BOLD + "动态 alpha 渐变(模拟对话框淡入):" + RESET)
        import math
        for frame in range(120):
            t = frame / 120.0
            a = 0.3 + 0.4 * (0.5 + 0.5 * math.sin(t * math.pi * 4))
            render_row(16, f"α={a:.2f}", a)
            time.sleep(0.03)

        sys.stdout.write(goto(19, 1) + RESET + BOLD + "结论:" + RESET)
        sys.stdout.write(goto(20, 1) + "  acecode 的 FTXUI Color 无 alpha 通道,遮罩只能完全盖住或完全透明。")
        sys.stdout.write(goto(21, 1) + "  opencode 的 RGBA(如 RGBA.fromInts(0,0,0,150))可做半透明遮罩,")
        sys.stdout.write(goto(22, 1) + "  配合 tint() 线性混色实现对话框/气泡的层次感。")
        sys.stdout.write(goto(23, 1) + "  grok 在 ratatui 之上自建 framebuffer 合成层达成同样效果。")
    finally:
        sys.stdout.write(goto(25, 1) + RESET + SHOW_CURSOR)
        sys.stdout.flush()


if __name__ == "__main__":
    main()
