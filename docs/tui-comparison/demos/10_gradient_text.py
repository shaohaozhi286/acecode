#!/usr/bin/env python3
"""10 - kimi-code 渐变品牌字(gradientText)

报告章节:3.4
acecode 现状:❌ 无渐变,FTXUI 每 cell 单色
有此能力:kimi-code -- theme/gradient-text.ts 的 gradientText():
  逐字符在 fromHex→toHex 间插值,给每个字符单独 ANSI truecolor,
  配合 accentBias 让渐变在首尾之间偏折。

演示:
  [A] Kimi 品牌风渐变(banner/logo 用)
  [B] 多组渐变对比(不同起止色)
  [C] accentBias 效果(渐变聚集在左侧)
  对照 acecode 的单色加粗文本(FTXUI 无法逐字符渐变)

实现:gradientText 的做法与 ANSI truecolor 完全兼容(逐字符 38;2;r;g;b),
FTXUI 之所以做不到,是因为它的 Element 按"整段文本"上色,不暴露逐字符色。
"""

import sys
from _term import init, RESET, BOLD, DIM

init()


def lerp(a, b, t):
    return round(a + (b - a) * t)


def hex_to_rgb(h):
    h = h.lstrip('#')
    return (int(h[0:2], 16), int(h[2:4], 16), int(h[4:6], 16))


def gradient_text(text, from_hex, to_hex, accent_bias=1.0):
    """复刻 kimi gradientText:逐字符插值 + accentBias 偏折。"""
    chars = list(text)
    n = len(chars)
    if n <= 1:
        return text
    c1 = hex_to_rgb(from_hex)
    c2 = hex_to_rgb(to_hex)
    out = []
    for i, ch in enumerate(chars):
        ratio = min(1.0, (i / (n - 1)) * accent_bias)
        r, g, b = lerp(c1[0], c2[0], ratio), lerp(c1[1], c2[1], ratio), lerp(c1[2], c2[2], ratio)
        out.append(f"\x1b[1m\x1b[38;2;{r};{g};{b}m{ch}")
    return "".join(out) + RESET


def main():
    print(BOLD + "=== kimi-code 渐变品牌字(gradientText)===" + RESET)
    print(DIM + "逐字符在 from→to 间插值 truecolor;kimi 用于 banner / 品牌元素" + RESET)
    print()

    print(BOLD + "[A] Kimi 品牌风渐变:" + RESET)
    print(" " + gradient_text("✦  Kimi CLI  ✦", "#1a8fff", "#7c3aed"))
    print(" " + gradient_text("    moonshot-ai / kimi-code  ", "#ff5a5f", "#ffb347"))
    print()

    print(BOLD + "[B] 多组渐变对比:" + RESET)
    pairs = [
        ("Kimi 蓝→紫", "#1a8fff", "#7c3aed"),
        ("青→绿",     "#00d2ff", "#3a7bd5"),
        ("粉→橙",     "#f857a6", "#ff5858"),
        ("绿→青",     "#00b09b", "#96c93d"),
    ]
    for label, a, b in pairs:
        print(f"  {label:<12} {gradient_text('██████ gradient text ██████', a, b)}")
    print()

    print(BOLD + "[C] accentBias 偏折(渐变聚集在左侧):" + RESET)
    print("  bias=1.0 " + gradient_text("ACECode TUI Discovery", "#ff0000", "#0000ff", 1.0))
    print("  bias=0.4 " + gradient_text("ACECode TUI Discovery", "#ff0000", "#0000ff", 0.4))
    print()

    print(BOLD + "对照 acecode(FTXUI 单色加粗):" + RESET)
    print("  " + f"\x1b[1m\x1b[38;2;90;140;255m✦  AceCode CLI  ✦{RESET}")
    print(DIM + "  ↑ FTXUI Element 按整段文本上色,不暴露逐字符色,做不到渐变。" + RESET)
    print()
    print(BOLD + "实现原理:" + RESET)
    print("  kimi 的 gradientText 用 chalk.hex().bold() 逐字符输出,ANSI truecolor 天然支持。")
    print("  FTXUI 若要做,需在 markdown_formatter 输出 Element 之前把文本按字符拆成多个")
    print("  text() 元素(每个带自己的颜色),或用自绘文本路径绕过 Element —— 类似 OSC 8 的坑。")


if __name__ == "__main__":
    main()
