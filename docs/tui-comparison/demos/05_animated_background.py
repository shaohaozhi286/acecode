#!/usr/bin/env python3
"""05 - 正弦呼吸动画背景

报告章节:3.4
acecode 现状:❌ 无背景动画(仅 compact_animation / thinking_heartbeat 文本)
有此能力:
  opencode -- BgPulse 逐帧程序化动画背景(正弦呼吸光圈、GO logo 高光扫动、临时降频 30fps)
  grok-build -- wave_brightness(tick, row) sin² 空间相位波(跨行扫过)+ pulse_brightness

演示两段:
  [A] 全屏 sin² 呼吸:背景亮度随 sin²(t) 循环(opencode BgPulse 风格)
  [B] 跨行相位波:每行相位偏移,形成自上而下扫过的波(grok wave_brightness 风格)

Ctrl+C 退出。
"""

import sys
import math
import time
from _term import init, RESET, BOLD, DIM, fg, bg, goto, CLEAR, HIDE_CURSOR, SHOW_CURSOR

init()

ROWS = 12
COLS = 60
TOP = 4


def lerp(a, b, t):
    return a + (b - a) * t


def main():
    sys.stdout.write(CLEAR + HIDE_CURSOR)
    try:
        sys.stdout.write(goto(1, 1) + BOLD +
                         "正弦呼吸动画背景(opencode BgPulse / grok wave_brightness)" + RESET)
        sys.stdout.write(goto(2, 1) + DIM + "[A] 全屏呼吸  [B] 跨行相位波   Ctrl+C 退出" + RESET)

        start = time.time()
        phase = "A"
        switch_at = 4.0
        while True:
            elapsed = time.time() - start
            if elapsed > switch_at:
                phase = "B" if phase == "A" else "A"
                switch_at = elapsed + 4.0
                label = "全屏 sin² 呼吸(opencode BgPulse)" if phase == "A" else "跨行相位波(grok wave_brightness)"
                sys.stdout.write(goto(3, 1) + DIM + f"[{phase}] {label}" + RESET + " " * 20)

            t = elapsed
            buf = []
            for r in range(ROWS):
                row = TOP + r
                if phase == "A":
                    # 全屏同相呼吸
                    brightness = 0.5 + 0.5 * math.sin(t * 2.0)
                else:
                    # 跨行相位波:每行相位偏移
                    brightness = 0.5 + 0.5 * math.sin(t * 2.0 - r * 0.5)
                # 背景色从深蓝到亮青
                br = lerp(20, 80, brightness)
                bgc = lerp(30, 180, brightness)
                bgr = lerp(15, 40, brightness)
                text_brightness = lerp(120, 255, brightness)
                buf.append(goto(row, 4))
                line_text = f"  row {r:2d}  ░▒▓█ wave demo █▓▒░  brightness={brightness:.2f}  "
                buf.append(bg(int(bgr), int(br), int(bgc)))
                buf.append(fg(int(text_brightness), int(text_brightness), int(text_brightness)))
                buf.append(line_text + RESET)
            sys.stdout.write("".join(buf))
            sys.stdout.flush()
            time.sleep(0.033)  # ~30fps
    except KeyboardInterrupt:
        pass
    finally:
        sys.stdout.write(goto(TOP + ROWS + 2, 1) + RESET + SHOW_CURSOR)
        sys.stdout.write("结论:背景动画需逐帧重绘每个 cell 的背景色,"
                         "FTXUI 的 Screen 重绘可做但 acecode 未实现;opencode/grok 把动画"
                         "与运行状态耦合进渲染管线(运行中 block 的 accent 柱做波浪/脉冲)。\n")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
