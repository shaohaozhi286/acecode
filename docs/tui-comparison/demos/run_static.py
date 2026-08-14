#!/usr/bin/env python3
"""批量运行非交互演示(01/02/04/07/08/09)。

动画类(05/06)和交互类(03)需单独跑。每个演示之间暂停按回车继续。
"""

import subprocess
import sys
import os

HERE = os.path.dirname(os.path.abspath(__file__))

DEMOS = [
    ("01_synchronized_output.py", "CSI 2026 同步输出对比"),
    ("02_osc8_hyperlinks.py",     "OSC 8 可点击超链接"),
    ("04_alpha_transparency.py",  "RGBA alpha 透明混色"),
    ("07_osc133_prompts.py",      "OSC 133 prompt 标记"),
    ("08_tool_row_dots.py",       "acecode ● 三态指示灯"),
    ("09_streaming_markdown.py",  "流式 markdown 增量渲染基准"),
    ("10_gradient_text.py",       "kimi 渐变品牌字"),
]


def main():
    for script, desc in DEMOS:
        path = os.path.join(HERE, script)
        print("\n" + "=" * 70)
        print(f"  {desc}")
        print("=" * 70)
        try:
            subprocess.run([sys.executable, path], cwd=HERE, check=False)
        except Exception as e:
            print(f"  [运行失败] {e}")
        if script != DEMOS[-1][0]:
            try:
                input("\n按回车继续下一个演示(Ctrl+C 中止)...")
            except (KeyboardInterrupt, EOFError):
                print()
                break


if __name__ == "__main__":
    main()
