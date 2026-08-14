#!/usr/bin/env python3
"""09 - 流式 markdown 增量渲染基准

报告章节:3.2
acecode 现状:❌ 整条重渲染 + redraw_pacer 限帧,无增量、无缓存层
有此能力:
  crush      -- glamour "stable-prefix":缓存安全前缀,只渲染尾部
  grok-build -- StreamingMarkdownRenderer checkpoint 冻结,只渲染活跃 tail
  opencode   -- OpenTUI streaming={true} 增量解析
  pi         -- 整条重解析但 cachedText/cachedWidth 缓存

本演示模拟流式逐 chunk 追加一个 markdown 文档,对比三种"渲染"策略的耗时:
  [朴素]   每次整条重新"渲染"(逐字符高亮扫描 + 行折叠)  -- 对应 acecode
  [前缀缓存] 已稳定的块渲染一次后缓存,只渲染活跃尾块      -- 对应 crush
  [checkpoint] 同上,块粒度冻结,只渲染活跃尾块            -- 对应 grok

关键:渲染开销做成 O(块长度)(逐字符状态机扫描,模拟 tokenizer+高亮+Element 构造),
缓存方法用块计数 O(1) 跳过已冻结块,而非 O(n) 字符串比较。这样朴素法的 O(n²) 累计
才会显出来。
"""

import sys
import time
from _term import init, RESET, BOLD, DIM, fg

init()

WIDTH = 80

# 模拟一个会不断增长的 markdown 文档(段落与代码块交替,每段较长)
PARA_TEXT = (
    "这是一段模拟的 markdown 段落,描述某次工具调用的结果与中间推理过程。"
    "包含若干行文字与代码引用,用于测量「渲染」工作量。流式场景下会逐 chunk 追加,"
    "每段约两百字,渲染时需逐字符做词法扫描与高亮状态机推进,再折行输出。"
)
CODE_TEXT = """```python
def render(doc: str, width: int) -> list[str]:
    out = []
    for raw in doc.split("\\n"):
        out.extend(wrap(raw, width))
    return out
```"""


def build_doc(n_chunks):
    """构造 n_chunks 段的文档(段落与代码块交替,\\n\\n 分块)。"""
    parts = []
    for i in range(n_chunks):
        parts.append(f"### 第 {i+1} 段\n\n{PARA_TEXT}\n")
        if i % 3 == 0:
            parts.append(CODE_TEXT + "\n")
    return "\n\n".join(parts)


# ---- "渲染"模拟:逐字符高亮扫描(状态机)+ 行折叠 ----

def render_block(block_text):
    """渲染单个块:逐字符推进高亮状态机 + 折行。O(块长度)。"""
    out = []
    for line in block_text.split("\n"):
        # 逐字符词法扫描(模拟 tokenizer + 高亮,这是主要开销)
        state = 0
        for ch in line:
            state = (state * 131 + ord(ch)) & 0x7FFFFFFF
        # 折行
        if not line:
            out.append("")
            continue
        tokens = line.replace("`", " ` ").split()
        cur = ""
        for tok in tokens:
            if len(cur) + len(tok) + 1 > WIDTH:
                out.append(cur)
                cur = tok
            else:
                cur = (cur + " " + tok) if cur else tok
        if cur:
            out.append(cur)
    return out


# ---- 三种策略 ----

class NaiveRenderer:
    """acecode 现状:整条重渲染。每次都渲染所有块。"""

    def render(self, text):
        blocks = text.split("\n\n")
        out = []
        for b in blocks:
            out.extend(render_block(b))
            out.append("")  # 块间空行
        return out


class StablePrefixRenderer:
    """crush stable-prefix:已稳定的块渲染一次缓存,只渲染活跃尾块。

    文本只增不减,所以"除最后一块外"都是稳定块。用块计数跟踪,避免 O(n) 字符串比较。
    """

    def __init__(self):
        self.frozen_lines = []      # 已缓存的前缀渲染行
        self.frozen_block_count = 0  # 已冻结的块数

    def render(self, text):
        blocks = text.split("\n\n")
        n = len(blocks)
        out = list(self.frozen_lines)
        # 新稳定化的块:之前是"活跃尾块",现在变成稳定块,渲染并冻结
        while self.frozen_block_count < n - 1:
            idx = self.frozen_block_count
            self.frozen_lines.extend(render_block(blocks[idx]))
            self.frozen_lines.append("")
            self.frozen_block_count += 1
        # 只渲染活跃尾块(最后一块)
        if n > 0:
            out = list(self.frozen_lines)
            out.extend(render_block(blocks[-1]))
            out.append("")
        return out


class CheckpointRenderer:
    """grok checkpoint:块粒度冻结,语义同 StablePrefix(本模拟中块即 checkpoint)。"""

    def __init__(self):
        self.frozen_lines = []
        self.frozen_block_count = 0

    def render(self, text):
        blocks = text.split("\n\n")
        n = len(blocks)
        while self.frozen_block_count < n - 1:
            idx = self.frozen_block_count
            self.frozen_lines.extend(render_block(blocks[idx]))
            self.frozen_lines.append("")
            self.frozen_block_count += 1
        out = list(self.frozen_lines)
        if n > 0:
            out.extend(render_block(blocks[-1]))
            out.append("")
        return out


def benchmark():
    print(BOLD + "=== 流式 markdown 增量渲染基准 ===" + RESET)
    print(DIM + "模拟逐 chunk 追加文档,测量每次「渲染」的耗时(微秒)" + RESET)
    print(DIM + "渲染开销 = 逐字符高亮状态机扫描 + 行折叠(O(块长度))" + RESET)
    print()
    print(f" {'chunk':<7} {'朴素(acecode)':<20} {'前缀缓存(crush)':<20} {'checkpoint(grok)':<20} {'朴素/缓存'}")
    print(f" {'-'*7} {'-'*20} {'-'*20} {'-'*20} {'-'*10}")

    naive = NaiveRenderer()
    prefix = StablePrefixRenderer()
    checkpoint = CheckpointRenderer()

    total_naive = total_prefix = total_ckpt = 0
    n_chunks = 60

    for i in range(1, n_chunks + 1):
        doc = build_doc(i)

        t0 = time.perf_counter_ns()
        naive.render(doc)
        t_naive = time.perf_counter_ns() - t0

        t0 = time.perf_counter_ns()
        prefix.render(doc)
        t_prefix = time.perf_counter_ns() - t0

        t0 = time.perf_counter_ns()
        checkpoint.render(doc)
        t_ckpt = time.perf_counter_ns() - t0

        total_naive += t_naive
        total_prefix += t_prefix
        total_ckpt += t_ckpt

        if i % 10 == 0 or i == 1:
            ratio = t_naive / max(t_prefix, 1)
            print(f" {i:<7} {t_naive/1000:>12.1f} µs   {t_prefix/1000:>12.1f} µs   "
                  f"{t_ckpt/1000:>12.1f} µs   {ratio:>6.1f}x")

    print()
    print(BOLD + "累计耗时(60 次流式更新):" + RESET)
    print(f"  朴素(acecode)     : {total_naive/1e6:>8.2f} ms")
    print(f"  前缀缓存(crush)   : {total_prefix/1e6:>8.2f} ms  "
          f"({total_naive/max(total_prefix,1):.1f}x 加速)")
    print(f"  checkpoint(grok)   : {total_ckpt/1e6:>8.2f} ms  "
          f"({total_naive/max(total_ckpt,1):.1f}x 加速)")
    print()
    print(BOLD + "结论:" + RESET)
    print("  朴素法每次渲染全部块,文档越长单次开销越大,累计 O(n²)。")
    print("  缓存法只渲染活跃尾块(O(1) 块/次),稳定块渲染一次后冻结,累计 O(n)。")
    print("  → acecode 的 format_markdown 每次流式更新整条重渲染 + 重建 FTXUI Element,")
    print("    长输出时 CPU 开销随文档长度线性增长,这是最值得补的差距(抄 crush/grok)。")
    print()
    print(DIM + "注:绝对数值随机器/Python 版本变化,重点是朴素法随 chunk 增长的斜率" + RESET)
    print(DIM + "    vs 缓存法的平坦斜率。实际 acecode 的 Element 构造开销更高,差距更大。" + RESET)


if __name__ == "__main__":
    benchmark()
