#!/usr/bin/env python3
"""03 - kitty keyboard 协议(交互式)

报告章节:3.6
acecode 现状:❌ 仅消费标准 CSI 修饰符(\\x1B[1;3A 等),未启用 kitty 协议
有此能力:opencode、pi、grok-build

kitty keyboard 协议启用后:
  - Enter        -> \\x1b[13u   (而非裸 \\r,可区分)
  - Shift+Enter  -> \\x1b[13;2u (修饰符 2=Shift)
  - Ctrl+A       -> \\x1b[97;5u (修饰符 5=Ctrl)
  - Alt+X        -> \\x1b[120;3u(修饰符 3=Alt)
  - 按键释放事件也可上报(flag 2)

acecode 在 Windows Terminal 上拿不到 Shift+Enter(WT 不支持 kitty),靠 IME 脏补丁;
pi 用原生 addon(GetAsyncKeyState)查全局修饰键补这个缺口。

演示:
  1. 打印键码参考表
  2. 进入 raw 模式 + 启用 kitty 协议,实时显示按键的原始字节
  3. 按 q 退出

注意:Windows Terminal 目前不支持 kitty 协议,启用后仍回退标准序列 -- 这本身就是
      演示的一部分(能看到 WT 发的是 \\x1b[1;3A 而非 \\x1b[13;2u)。
      kitty / WezTerm / Ghostty 上能看到完整 CSI u 序列。
"""

import sys
from _term import (
    init, RESET, BOLD, DIM, fg, CLEAR, HIDE_CURSOR, SHOW_CURSOR,
    raw_mode, read_key, KITTY_PUSH, KITTY_POP, goto,
)

init()


def fmt_bytes(b):
    """把字节格式化为可读的转义序列表示。"""
    out = []
    for byte in b:
        if byte == 0x1b:
            out.append(fg(255, 100, 100) + "\\x1b" + RESET)
        elif byte == 0x0d:
            out.append(fg(255, 200, 100) + "\\r" + RESET)
        elif byte == 0x0a:
            out.append(fg(255, 200, 100) + "\\n" + RESET)
        elif byte == 0x07:
            out.append(fg(255, 200, 100) + "\\x07" + RESET)
        elif 32 <= byte < 127:
            out.append(chr(byte))
        else:
            out.append(fg(200, 200, 200) + f"\\x{byte:02x}" + RESET)
    return "".join(out)


def reference_table():
    print(BOLD + "=== 键码参考:标准序列 vs kitty CSI u 序列 ===" + RESET)
    print()
    print(f" {'按键':<16} {'标准(acecode 消费)':<24} {'kitty 协议':<20} {'可区分?'}")
    print(f" {'-'*16} {'-'*24} {'-'*20} {'-'*8}")
    rows = [
        ("Enter",      "\\r",              "\\x1b[13u",      "✓(与 Shift 区分)"),
        ("Shift+Enter","\\r(同 Enter!)",   "\\x1b[13;2u",    "✓"),
        ("Ctrl+A",     "\\x01",            "\\x1b[97;5u",    "✓"),
        ("Alt+X",      "\\x1bx",           "\\x1b[120;3u",   "✓"),
        ("Shift+Tab",  "\\x1b[Z",          "\\x1b[9;2u",     "✓(标准也行)"),
        ("Ctrl+Shift+↑","\\x1b[1;6A",      "\\x1b[1;6;1u",   "✓ + 事件类型"),
        ("释放事件",   "(无)",             "\\x1b[...;1u",   "✓(flag 2)"),
    ]
    for key, std, kitty, note in rows:
        print(f" {key:<16} {fg(180,180,180)}{std:<24}{RESET} {fg(120,200,255)}{kitty:<20}{RESET} {note}")
    print()
    print(DIM + " acecode 现状:消费 \\x1B[1;3A(Alt+↑)等标准序列,但未启用 kitty 协议," + RESET)
    print(DIM + " 所以 Shift+Enter 在多数终端拿不到(与 Enter 都是 \\r)。pi 用原生 addon 查" + RESET)
    print(DIM + " GetAsyncKeyState 补这个缺口;opencode/grok 直接用 kitty 协议。" + RESET)
    print()


def live_capture():
    print(BOLD + "=== 实时捕获(按 q 退出)===" + RESET)
    print(DIM + " 试试 Enter / Shift+Enter / Ctrl+L / Alt+X / 方向键,看原始字节" + RESET)
    print(DIM + " 启用了 kitty 协议(\\x1b[>15u);不支持的终端会回退标准序列" + RESET)
    print()

    # 启用 kitty 协议
    sys.stdout.write(KITTY_PUSH)
    sys.stdout.flush()

    row = 10
    try:
        while True:
            sys.stdout.write(goto(row, 1) + fg(120, 200, 255) + " ❯ " + RESET + "等待按键..." + " " * 30)
            sys.stdout.flush()
            b = read_key()
            if not b:
                continue
            display = fmt_bytes(b)
            # q 退出(但避免误判带修饰的 q)
            if b == b"q":
                break
            sys.stdout.write(goto(row, 1) + fg(120, 200, 255) + " ❯ " + RESET +
                             display + "  " + DIM + f"({len(b)} bytes)" + RESET + " " * 10)
            sys.stdout.flush()
            row += 1
            if row > 25:
                row = 10
    finally:
        sys.stdout.write(KITTY_POP + "\n")
        sys.stdout.flush()


def main():
    sys.stdout.write(CLEAR + HIDE_CURSOR)
    try:
        reference_table()
        print(BOLD + "进入 raw 模式捕获按键..." + RESET)
        with raw_mode():
            live_capture()
    finally:
        sys.stdout.write(SHOW_CURSOR + RESET + "\n")
        sys.stdout.flush()
        print()
        print(BOLD + "观察要点:" + RESET)
        print("  - 在 kitty/WezTerm/Ghostty 上,Shift+Enter 应显示 \\x1b[13;2u")
        print("  - 在 Windows Terminal 上,Shift+Enter 可能仍是 \\r(kitty 不支持)--> 这正是 acecode 的困境")
        print("  - acecode 若启用 kitty(或至少 modifyOtherKeys),可在支持的终端可靠区分修饰键")


if __name__ == "__main__":
    main()
