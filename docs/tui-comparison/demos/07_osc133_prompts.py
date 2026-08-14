#!/usr/bin/env python3
"""07 - OSC 133 Prompt 语义标记

报告章节:3.6
acecode 现状:❌ 无
有此能力:pi(用 \\x1b]133;A/B/C 包裹每个 turn,支持 scrollToPrompt 上下跳转)

OSC 133 标记:
  \\x1b]133;A\\x07  prompt 开始(用户输入区起点)
  \\x1b]133;B\\x07  prompt 结束 / 输出开始
  \\x1b]133;C\\x07  输出结束

支持的终端(kitty / WezTerm / Ghostty)提供 Cmd+Shift+↑/↓ 在 prompt 边界间跳转,
类似 shell 里的 prompt 边界导航。pi 借此在 transcript 里按"回合"快速跳转。

演示:打印几轮对话,每轮用 OSC 133 标记边界。在 kitty/WezTerm 里试 Cmd+Shift+↑/↓。
然后对比 acecode 的无标记纯文本(无法按回合跳转,只能逐行滚)。
"""

import sys
from _term import init, RESET, fg, BOLD, DIM, fg256

init()

A = "\x1b]133;A\x07"  # prompt start
B = "\x1b]133;B\x07"  # prompt end / output start
C = "\x1b]133;C\x07"  # output end


def user_turn(label, text):
    # 用户输入区: A ... B
    return (A + fg(86, 156, 214) + BOLD + f"❯ {label}: " + text + RESET + B)


def assistant_out(text):
    return "\n" + fg(220, 220, 170) + text + RESET + "\n" + C


def main():
    print(BOLD + "=== OSC 133 Prompt 语义标记(pi 有,acecode 无)===" + RESET)
    print()
    print(DIM + "在 kitty/WezTerm/Ghostty 里试 Cmd+Shift+↑/↓(或终端的 prompt 跳转快捷键)" + RESET)
    print(DIM + "光标会在每个 ❯ 之间跳转;acecode 无此标记,只能逐行滚。" + RESET)
    print()

    # 三轮带标记的对话
    sys.stdout.write(user_turn("用户", "解释一下同步输出"))
    sys.stdout.write(assistant_out("CSI 2026 同步输出把整帧原子化提交,终端不会显示半帧状态。"))
    sys.stdout.write("\n")

    sys.stdout.write(user_turn("用户", "kitty keyboard 协议有什么用"))
    sys.stdout.write(assistant_out("它让 Shift+Enter、Ctrl+组合等修饰键以明确的 CSI u 序列上报,"
                                   "而不是无法区分的裸 \\r。"))
    sys.stdout.write("\n")

    sys.stdout.write(user_turn("用户", "acecode 为何没接入"))
    sys.stdout.write(assistant_out("FTXUI 的 Element 网格模型难以承载 prompt 边界这类跨 cell 的"
                                   "语义标记,且 acecode 未在 rasterize 后注入 OSC 133。"))
    sys.stdout.write("\n\n")

    print(BOLD + "=== acecode 现状:无标记纯文本(无法按回合跳转)===" + RESET)
    print()
    print(fg(86, 156, 214) + BOLD + "❯ 用户: 解释一下同步输出" + RESET)
    print(fg(220, 220, 170) + "CSI 2026 同步输出把整帧原子化提交。" + RESET)
    print()
    print(fg(86, 156, 214) + BOLD + "❯ 用户: kitty keyboard 协议有什么用" + RESET)
    print(fg(220, 220, 170) + "它让修饰键以明确的 CSI u 序列上报。" + RESET)
    print()
    print(DIM + "↑ 视觉上一样,但缺 OSC 133 标记 -- 终端不知道哪里是回合边界,无法提供跳转。" + RESET)
    print()
    print(BOLD + "接入难度:低。" + RESET)
    print("OSC 133 是纯文本序列,不依赖 cell 网格。acecode 只需在 user 消息渲染前后、"
          "assistant 输出前后各 print 一对标记即可(在 FTXUI 外层或 transcript 行输出时注入)。")


if __name__ == "__main__":
    main()
