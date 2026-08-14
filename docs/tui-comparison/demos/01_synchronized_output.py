#!/usr/bin/env python3
"""01 - CSI 2026 同步输出(Synchronized Update)对比

报告章节:3.1 / 3.6
acecode 现状:❌ 无,FTXUI 用 \\033[1A 光标回退逐帧重绘,无同步输出协议
有此协议:pi(TuiMainScreen/TuiAltScreen 整帧包 CSI 2026)、grok-build(draw.rs 每帧包)

演示:快速重绘一个 10 行计数块。
  阶段 A:不加同步 -- 可见闪烁/撕裂(光标在行间跳动可见)
  阶段 B:用 \\x1b[?2026h...\\x1b[?2026l 包裹整帧 -- 平滑无闪烁

在刷新率高的终端上差异最明显;慢终端上 A 阶段撕裂更明显。
"""

import sys
import time
from _term import (
    init, CLEAR, RESET, HIDE_CURSOR, SHOW_CURSOR,
    SYNC_BEGIN, SYNC_END, goto, CLEAR_LINE, fg, bg, BOLD,
)

init()

LINES = 10
COLS = 48
TOP = 3


def render_block(frame, sync):
    """渲染 10 行计数块。每行一个独立计数器,模拟 TUI 多区域同时刷新。"""
    parts = []
    if sync:
        parts.append(SYNC_BEGIN)
    for i in range(LINES):
        row = TOP + i
        v = (frame * (i + 1)) % 1000
        bar = "█" * (v % 30) + "░" * (30 - v % 30)
        # 不同行不同颜色,模拟多组件
        c = [(100, 200, 255), (255, 200, 100), (180, 255, 140), (220, 140, 255)][i % 4]
        parts.append(goto(row, 4) + CLEAR_LINE + bg(30, 30, 40) + fg(*c) +
                     f" line {i:2d} | {bar} | {v:4d} " + RESET)
    if sync:
        parts.append(SYNC_END)
    sys.stdout.write("".join(parts))
    sys.stdout.flush()


def phase(label, sync, frames=80):
    sys.stdout.write(goto(1, 1) + CLEAR_LINE + BOLD + f" {label}" + RESET)
    for f in range(frames):
        render_block(f, sync)
        time.sleep(0.016)  # ~60fps
    # 留白
    time.sleep(0.4)


def main():
    sys.stdout.write(CLEAR + HIDE_CURSOR)
    try:
        phase("阶段 A: 无同步输出(acecode 现状 -- 注意闪烁/撕裂)        ", sync=False)
        phase("阶段 B: CSI 2026 同步输出(pi/grok -- 平滑无闪烁)          ", sync=True)
        sys.stdout.write(goto(TOP + LINES + 2, 1) + RESET +
                         "对比结论:同步输出把整帧原子化提交,终端不会出现半帧状态。\n"
                         "acecode 的 FTXUI 用 \\033[1A 光标回退逐行重绘,在高刷新率/慢终端上会闪烁。\n")
    finally:
        sys.stdout.write(SHOW_CURSOR + RESET + "\n")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
