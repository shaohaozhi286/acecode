#!/usr/bin/env python3
"""08 - acecode 工具行 ● 三态指示灯(本项目独有优势)

报告章节:二 / 四.3
acecode 现状:✅ 独有风格 -- compute_tool_call_dots FIFO 配对 tool_call ↔ tool_result
  ● 灰=执行中 / 绿=成功 / 红=失败,pascal_case_tool_name 加粗,参数预览

对比其他项目:
  opencode: 消息卡片左侧竖线,工具为文本 part
  pi:      toolPendingBg/toolSuccessBg/toolErrorBg 背景色块
  grok:    左侧 accent 竖线(accent_bar ┃),block 折叠/截断

演示:模拟一段 transcript,展示 acecode 的:
  - 工具调用行 ● ToolName(args)
  - 配对结果行(缩进 └ + 摘要)
  - 三态指示灯(执行中灰 / 成功绿 / 失败红)
  - 孤儿调用(无结果,保持灰)
  - 并行调用 FIFO 配对
"""

import sys
import time
from _term import init, RESET, BOLD, DIM, fg, goto, CLEAR_LINE, HIDE_CURSOR, SHOW_CURSOR

init()

# 指示灯颜色(对齐 acecode 三态语义)
DOT_WORKING = fg(160, 160, 160)   # 灰=执行中
DOT_SUCCESS = fg(120, 200, 120)   # 绿=成功
DOT_FAILED = fg(230, 100, 100)    # 红=失败
TOOL_COLOR = fg(180, 140, 220)    # syntax.preproc 紫
SUMMARY = DIM
RESULT_PREFIX = "  └ "


def tool_call(name, args, dot=DOT_WORKING):
    return f"{dot}●{RESET} {TOOL_COLOR}{BOLD}{name}{RESET}{DIM}({args}){RESET}"


def tool_result(text, ok=True):
    color = fg(160, 200, 160) if ok else fg(230, 130, 130)
    return f"{RESULT_PREFIX}{color}{text}{RESET}"


def line(text):
    sys.stdout.write(text + "\n")


def main():
    print(BOLD + "=== acecode 工具行 ● 三态指示灯(独有优势)===" + RESET)
    print(DIM + "对齐 Claude Code 风格: ● ToolName(args) + 缩进 └ 结果摘要" + RESET)
    print()

    print(BOLD + "[1] 三态指示灯" + RESET)
    line(tool_call("Bash", "command=\"npm test\"", dot=DOT_WORKING) + "   " + DIM + "← 执行中(灰)" + RESET)
    time.sleep(0.3)
    line(tool_result("3 passed", ok=True) + "   " + DIM + "← 成功(绿)" + RESET)
    line(tool_call("FileWrite", "path=src/main.cpp", dot=DOT_WORKING))
    time.sleep(0.3)
    line(tool_result("Error: permission denied", ok=False) + "   " + DIM + "← 失败(红)" + RESET)
    line(tool_call("Grep", "pattern=TODO") + "   " + DIM + "← 孤儿(无结果,保持灰)" + RESET)
    print()

    print(BOLD + "[2] 并行调用 FIFO 配对(compute_tool_call_dots)" + RESET)
    print(DIM + "三个并行读工具同时发出,结果按完成顺序回收,指示灯逐个变绿:" + RESET)
    line(tool_call("FileRead", "path=a.cpp"))
    line(tool_call("FileRead", "path=b.cpp"))
    line(tool_call("FileRead", "path=c.cpp"))
    # 模拟逐个完成 -- 但因为是 FIFO 配对,只有最前的那个能变绿
    time.sleep(0.4)
    # 用回退光标重写第一行为成功
    sys.stdout.write("\x1b[4A")  # 上移 4 行到第一个 FileRead
    line(tool_call("FileRead", "path=a.cpp", dot=DOT_SUCCESS))
    sys.stdout.write("\x1b[3B")  # 回到下面
    time.sleep(0.4)
    print()

    print(BOLD + "[3] 参数预览与 task_complete" + RESET)
    line(tool_call("TaskComplete", "summary=\"重构完成\""))
    time.sleep(0.3)
    line(tool_result("● Done for 4.2s", ok=True))
    print()

    print(BOLD + "对比其他项目:" + RESET)
    print(f"  opencode: 消息卡片左侧 {fg(180,140,220)}┃{RESET} 竖线,工具是文本 part,无三态灯")
    print(f"  pi:      toolPendingBg/SuccessBg/ErrorBg {fg(60,60,80)}背景色块{RESET},无指示灯")
    print(f"  grok:    {fg(180,140,220)}┃{RESET} accent 竖线 + block 折叠/截断,无 FIFO 配对语义")
    print()
    print(BOLD + "acecode 这套是自创的紧凑 transcript 风格,信息密度高:" + RESET)
    print("  - 指示灯三态一眼看出工具成败")
    print("  - FIFO 配对让并行调用的「谁还没完成」一目了然")
    print("  - Ctrl+O 全局展开 / Ctrl+E 逐行展开长输出")


if __name__ == "__main__":
    main()
