#!/usr/bin/env python3
"""06 - 五种 Spinner 同屏对比

报告章节:3.4
acecode 现状:❌ 仅 compact_animation,无丰富 spinner
其他项目:
  opencode -- Knight Rider 扫描光带(逐像素 alpha 渐变拖尾、菱形/方块、双向扫描+端点停留)
  grok-build -- 三套帧集:braille ⠋⠙⠹⠸⠼⠴⠦⠧ / dot ⋅ : ⸬ ⁙ / monitor ○ ◎ ◉ ◎
  pi -- loader.ts 盲文帧(⠋⠙⠹...,80ms)

演示:5 个 spinner 同屏旋转,标注来源。Ctrl+C 退出。
"""

import sys
import time
from _term import init, RESET, BOLD, DIM, fg, goto, CLEAR, HIDE_CURSOR, SHOW_CURSOR

init()

# ---- 帧集 ----
BRAILLE = ["⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧"]          # grok / pi
DOT = ["⋅", ":", "⸬", "⁙"]                                    # grok
MONITOR = ["○", "◎", "◉", "◎"]                                # grok

# Knight Rider 扫描光带(opencode ui/spinner.ts 风格)
KR_WIDTH = 10
def knight_rider(frame):
    pos = frame % (KR_WIDTH * 2 - 2)
    if pos >= KR_WIDTH:
        pos = KR_WIDTH * 2 - 2 - pos
    bar = ["░"] * KR_WIDTH
    # 主光点 + 拖尾(alpha 渐变用不同亮度字符模拟)
    trail = ["█", "▓", "▒", "░"]
    for i, ch in enumerate(trail):
        idx = pos - i
        if 0 <= idx < KR_WIDTH:
            bar[idx] = ch
    return "".join(bar)

# acecode 风格:静态 ●(或 compact_animation 的简单脉冲)
def acecode_dot(frame):
    # 模拟 compact_animation: ● 亮度脉冲
    bright = (frame % 4) in (0, 1)
    return fg(200, 200, 200) if bright else fg(120, 120, 120) + "●" + RESET


SPINNERS = [
    ("acecode", "●(compact_animation 脉冲)", lambda f: acecode_dot(f)),
    ("grok", "braille ⠋⠙⠹⠸⠼⠴⠦⠧", lambda f: fg(120, 200, 255) + BRAILLE[f % len(BRAILLE)] + RESET),
    ("grok", "dot ⋅ : ⸬ ⁙", lambda f: fg(255, 200, 100) + DOT[f % len(DOT)] + RESET),
    ("grok", "monitor ○ ◎ ◉ ◎", lambda f: fg(180, 255, 140) + MONITOR[f % len(MONITOR)] + RESET),
    ("opencode", "Knight Rider 扫描光带", lambda f: fg(255, 80, 80) + knight_rider(f) + RESET),
]


def main():
    sys.stdout.write(CLEAR + HIDE_CURSOR)
    try:
        sys.stdout.write(goto(1, 1) + BOLD + "Spinner 同屏对比(acecode 无丰富 spinner)" + RESET)
        sys.stdout.write(goto(2, 1) + DIM + "Ctrl+C 退出" + RESET)
        frame = 0
        while True:
            buf = []
            for i, (src, desc, fn) in enumerate(SPINNERS):
                row = 4 + i
                buf.append(goto(row, 3))
                buf.append(f"{fn(frame)}  {BOLD}{src:<10}{RESET} {DIM}{desc}{RESET}")
            # 状态文本
            buf.append(goto(4 + len(SPINNERS) + 1, 3))
            buf.append(DIM + f"frame={frame}  各 spinner 独立帧率/方向" + RESET + " " * 20)
            sys.stdout.write("".join(buf))
            sys.stdout.flush()
            frame += 1
            time.sleep(0.08)  # ~12fps,盲文帧 80ms 对齐
    except KeyboardInterrupt:
        pass
    finally:
        sys.stdout.write(goto(4 + len(SPINNERS) + 3, 1) + RESET + SHOW_CURSOR)
        sys.stdout.write("结论:acecode 仅 compact_animation,无多套 spinner 帧集;\n"
                         "opencode 的 Knight Rider 用逐像素 alpha 渐变(FTXUI 不透明色做不到拖尾衰减),\n"
                         "grok 三套帧集按 legacy ConHost 回退 ASCII。\n")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
