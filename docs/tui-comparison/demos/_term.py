"""Shared terminal helpers for TUI comparison demos.

跨平台 ANSI 启用 + 原始输入模式(交互演示用)。
所有演示 import 本模块以获得统一的颜色/光标/协议常量与清理逻辑。
"""

import os
import sys
import ctypes
import contextlib


# ---------------------------------------------------------------------------
# Windows: 启用 VT 处理 + UTF-8 输出
# ---------------------------------------------------------------------------

def enable_vt():
    """Windows 上开启 ENABLE_VIRTUAL_TERMINAL_PROCESSING + UTF-8 输出 CP。

    不开启时,ANSI 转义序列会被原样打印(看到一堆 \\x1b[...),且盲文/box-drawing
    字符在非 UTF-8 代码页下会乱码。grok-build 的 configure_windows_console() 做同样的事。
    """
    if os.name != 'nt':
        return
    try:
        kernel32 = ctypes.windll.kernel32
        ENABLE_VIRTUAL_TERMINAL_PROCESSING = 0x0004
        # STD_OUTPUT_HANDLE = -11, STD_ERROR_HANDLE = -12
        for handle in (-11, -12):
            h = kernel32.GetStdHandle(handle)
            mode = ctypes.c_uint32()
            if kernel32.GetConsoleMode(h, ctypes.byref(mode)):
                kernel32.SetConsoleMode(h, mode.value | ENABLE_VIRTUAL_TERMINAL_PROCESSING)
        kernel32.SetConsoleOutputCP(65001)  # CP_UTF8
    except Exception:
        pass


# ---------------------------------------------------------------------------
# ANSI / 终端协议常量
# ---------------------------------------------------------------------------

RESET = "\x1b[0m"
BOLD = "\x1b[1m"
DIM = "\x1b[2m"
ITALIC = "\x1b[3m"
UNDERLINE = "\x1b[4m"

CLEAR = "\x1b[2J\x1b[H"
CLEAR_LINE = "\x1b[2K"

HIDE_CURSOR = "\x1b[?25l"
SHOW_CURSOR = "\x1b[?25h"

ALT_SCREEN = "\x1b[?1049h"
MAIN_SCREEN = "\x1b[?1049l"

# CSI 2026 同步输出(Begin/End Synchronized Update) -- pi / grok 用
SYNC_BEGIN = "\x1b[?2026h"
SYNC_END = "\x1b[?2026l"

# kitty keyboard 协议 -- push flags 并设置(Ps=flags); pop 恢复
# flags: 1=disambiguate, 2=event-type, 4=report-all-keys, 8=alternate-keys
KITTY_PUSH = "\x1b[>15u"   # 1|2|4|8
KITTY_POP = "\x1b[<u"

# xterm modifyOtherKeys -- acecode 消费标准序列但未显式启用
MODIFY_OTHER_KEYS_ON = "\x1b[>4;2m"
MODIFY_OTHER_KEYS_OFF = "\x1b[>4m"

# OSC 52 剪贴板(acecode 右键复制用)
def osc52_copy(text: str) -> str:
    import base64
    return f"\x1b]52;c;{base64.b64encode(text.encode('utf-8')).decode()}\x07"


# ---------------------------------------------------------------------------
# 颜色辅助(truecolor)
# ---------------------------------------------------------------------------

def fg(r, g, b):
    return f"\x1b[38;2;{r};{g};{b}m"

def bg(r, g, b):
    return f"\x1b[48;2;{r};{g};{b}m"

def fg256(n):
    return f"\x1b[38;5;{n}m"

def bg256(n):
    return f"\x1b[48;5;{n}m"

def goto(row, col):
    return f"\x1b[{row};{col}H"

def up(n=1):
    return f"\x1b[{n}A"

def down(n=1):
    return f"\x1b[{n}B"


def blend(c1, c2, t):
    """线性混色(模拟 alpha 合成):t=0 全 c1,t=1 全 c2。

    opencode/grok 的 RGBA 透明在 framebuffer 层做 per-pixel alpha 合成;
    ANSI 无法逐 cell 设 alpha,这里用预混色模拟视觉效果(acecode 的 FTXUI 做不到)。
    """
    return tuple(round(c1[i] + (c2[i] - c1[i]) * t) for i in range(3))


# ---------------------------------------------------------------------------
# 原始输入模式(交互演示用,跨平台)
# ---------------------------------------------------------------------------

@contextlib.contextmanager
def raw_mode():
    """进入原始输入模式(cbreak),退出时恢复。

    Windows: SetConsoleMode 清除 ECHO/LINE_INPUT/PROCESSED_INPUT,
             开启 ENABLE_VIRTUAL_TERMINAL_INPUT(让修饰键组合以 VT 序列上报)。
             这正是 pi 的 win32-console-mode.node 做的事。
    POSIX: termios tty.setcbreak。
    """
    if os.name == 'nt':
        kernel32 = ctypes.windll.kernel32
        STD_INPUT_HANDLE = -10
        ENABLE_VIRTUAL_TERMINAL_INPUT = 0x0200
        ENABLE_ECHO_INPUT = 0x0004
        ENABLE_LINE_INPUT = 0x0002
        ENABLE_PROCESSED_INPUT = 0x0001
        h = kernel32.GetStdHandle(STD_INPUT_HANDLE)
        old = ctypes.c_uint32()
        kernel32.GetConsoleMode(h, ctypes.byref(old))
        new = old.value & ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT)
        new |= ENABLE_VIRTUAL_TERMINAL_INPUT
        kernel32.SetConsoleMode(h, new)
        try:
            yield
        finally:
            kernel32.SetConsoleMode(h, old.value)
            sys.stdout.write(RESET + SHOW_CURSOR)
            sys.stdout.flush()
    else:
        import termios
        import tty
        fd = sys.stdin.fileno()
        old = termios.tcgetattr(fd)
        try:
            tty.setcbreak(fd)
            yield
        finally:
            termios.tcsetattr(fd, termios.TCSADRAIN, old)
            sys.stdout.write(RESET + SHOW_CURSOR)
            sys.stdout.flush()


def read_key():
    """读一个按键(可能多字节),返回原始字节字符串。非阻塞超时返回 b''。"""
    import select
    ch = sys.stdin.buffer.read(1)
    if not ch:
        return b''
    # 读余下的转义序列(非阻塞,短超时)
    seq = ch
    while True:
        r, _, _ = select.select([sys.stdin], [], [], 0.02)
        if not r:
            break
        c = sys.stdin.buffer.read(1)
        if not c:
            break
        seq += c
    return seq


# ---------------------------------------------------------------------------
# 公共入口:每个 demo 调用一次
# ---------------------------------------------------------------------------

def init():
    enable_vt()
    # 确保 stdout 用 UTF-8(Windows 上 sys.stdout 可能是 cp936)
    if os.name == 'nt':
        try:
            sys.stdout.reconfigure(encoding='utf-8')
        except Exception:
            pass
