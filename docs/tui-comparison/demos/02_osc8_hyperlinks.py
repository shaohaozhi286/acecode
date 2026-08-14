#!/usr/bin/env python3
"""02 - OSC 8 可点击超链接对比

报告章节:3.6
acecode 现状:⚠️ 有检测代码(mardown_formatter.cpp:78 terminal_supports_hyperlinks)
  但 make_hyperlink() 注明 "FTXUI Elements 无法内嵌 OSC 8",只返回纯文本 -- 链接不可点击
有此能力:opencode、pi、grok-build(grok 还自动 linkify 文件路径/URL)

演示:打印几条链接(URL / file:// 路径),对比:
  上半:OSC 8 超链接 -- 支持的终端里 Ctrl/Cmd+Click 可打开
  下半:acecode 现状 -- 纯色文本,不可点击(下划线只是颜色装饰)

OSC 8 格式: \\x1b]8;;URL\\x07 显示文本 \\x1b]8;;\\x07
"""

import sys
import os
from _term import init, RESET, fg, UNDERLINE, BOLD, DIM

init()


def osc8(url, text):
    return f"\x1b]8;;{url}\x07{text}\x1b]8;;\x07"


def main():
    sample_file = os.path.abspath(__file__).replace("\\", "/")
    repo_root = os.path.dirname(os.path.dirname(os.path.dirname(sample_file)))
    repo_file = "file:///" + os.path.join(repo_root, "src", "main.cpp").replace("\\", "/")

    print(BOLD + "=== OSC 8 可点击超链接(opencode / pi / grok 有此能力)===" + RESET)
    print()
    print(" " + osc8("https://github.com/charmbracelet/crush", "crush (GitHub)"))
    print(" " + osc8("https://sw.kovidgoyal.net/kitty/keyboard-protocol/", "kitty keyboard 协议文档"))
    print(" " + osc8(repo_file, "src/main.cpp(点击在编辑器打开)"))
    print(" " + osc8("https://example.com/path?q=1", "带查询参数的 URL"))
    print()
    print(DIM + " ↑ 在 Windows Terminal / kitty / WezTerm / iTerm2 里 Ctrl+Click(或 Cmd+Click)可打开" + RESET)
    print()

    print(BOLD + "=== acecode 现状:纯色文本,不可点击 ===" + RESET)
    print()
    print(" " + fg(100, 180, 255) + UNDERLINE + "crush (GitHub)" + RESET +
          DIM + "    https://github.com/charmbracelet/crush" + RESET)
    print(" " + fg(100, 180, 255) + UNDERLINE + "kitty keyboard 协议文档" + RESET +
          DIM + "    https://sw.kovidgoyal.net/kitty/keyboard-protocol/" + RESET)
    print(" " + fg(100, 180, 255) + UNDERLINE + "src/main.cpp" + RESET +
          DIM + "    " + repo_file + RESET)
    print()
    print(DIM + " ↑ acecode 的 markdown_formatter 检测到支持 OSC 8 的终端后,"
                 "仍只渲染带下划线的蓝色文本 -- make_hyperlink() 因 FTXUI Element" + RESET)
    print(DIM + "   网格模型无法内嵌原始 OSC 8 序列而退化。这是架构限制,非简单 bug。" + RESET)
    print()
    print(BOLD + "为何 FTXUI 做不到:" + RESET)
    print(" FTXUI 的渲染管线把组件树 rasterize 成字符网格(Screen),每个 cell 只存"
          "字符+颜色,无法在某段文本中间插入 OSC 8 的开始/结束标记(那会破坏网格对齐)。")
    print(" opencode/grok 用自己的 framebuffer/ratatui,可以在 cell 之外维护一个 link 层"
          "(grok 的 flush_with_links 把链接变化与 cell 变化一起 diff)。")
    print()
    print(BOLD + "acecode 可行的接入路径:" + RESET)
    print(" 1. 在 FTXUI Screen rasterize 之后、写终端之前,做一遍后处理:"
          "扫描带 link 标记的 cell 区间,用 OSC 8 包裹(需 FTXUI 暴露 link 元数据)。")
    print(" 2. 或在 markdown_formatter 阶段直接产出原始 ANSI 文本(绕过 FTXUI Element),"
          "像 grok 那样在 transcript 区用自绘文本而非 FTXUI 组件。")


if __name__ == "__main__":
    main()
