# TUI 对比效果演示

配合 [`../report.md`](../report.md) 的可运行效果演示。每个脚本自包含,演示一个终端效果,并标注**哪些项目有、acecode 现状如何**。

## 运行环境

- Python 3.8+(Windows 自带 3.12 可用)
- 推荐终端:**Windows Terminal** / kitty / WezTerm / iTerm2 / Ghostty
- 老版 cmd.exe / conhost 部分效果(盲文、truecolor)会降级

## 运行

```bash
cd docs/tui-comparison/demos
python 01_synchronized_output.py
python 02_osc8_hyperlinks.py
python 03_kitty_keyboard.py      # 交互式,按 q 退出
python 04_alpha_transparency.py
python 05_animated_background.py  # 动画,Ctrl+C 退出
python 06_spinner_showcase.py      # 动画,Ctrl+C 退出
python 07_osc133_prompts.py
python 08_tool_row_dots.py
python 09_streaming_markdown.py
```

或一键跑非交互的(1/2/4/7/8/9):

```bash
python run_static.py
```

## 演示与报告对照

| 演示 | 演示的效果 | 报告章节 | acecode 现状 |
|---|---|---|---|
| `01_synchronized_output` | CSI 2026 同步输出消除闪烁 | 3.1 / 3.6 | ❌ 无,FTXUI 光标回退 |
| `02_osc8_hyperlinks` | OSC 8 可点击超链接 | 3.6 | ⚠️ 有检测代码但不发射(FTXUI 限制) |
| `03_kitty_keyboard` | kitty keyboard 协议键码 | 3.6 | ❌ 仅消费标准 CSI 修饰符 |
| `04_alpha_transparency` | RGBA alpha 混色(透明叠层) | 3.1 / 3.4 | ❌ FTXUI 颜色不透明 |
| `05_animated_background` | 正弦呼吸动画背景 | 3.4 | ❌ 无 |
| `06_spinner_showcase` | 五种 spinner 同屏对比 | 3.4 | ❌ 仅 compact_animation |
| `07_osc133_prompts` | OSC 133 prompt 语义标记 | 3.6 | ❌ 无 |
| `08_tool_row_dots` | `●` 三态指示灯 | 二 / 四.3 | ✅ acecode 独有优势 |
| `09_streaming_markdown` | 流式 markdown 增量渲染基准 | 3.2 | ❌ 整条重渲染 |

## 各演示说明

### 01_synchronized_output.py
**CSI 2026 同步输出**(Begin/End Synchronized Update)。快速重绘一个 10 行块,先不加同步(可见闪烁/撕裂),再加 `\x1b[?2026h...\x1b[?2026l` 包裹(平滑)。pi 和 grok-build 用此协议;acecode 的 FTXUI 用 `\033[1A` 光标回退,无同步输出。

### 02_osc8_hyperlinks.py
**OSC 8 超链接**:`\x1b]8;;URL\x07text\x1b]8;;\x07`,在支持的终端里 Ctrl/Cmd+Click 打开。acecode 的 `markdown_formatter.cpp:78` 有 `terminal_supports_hyperlinks()` 检测,但 `make_hyperlink()` 因 *"FTXUI Elements 无法内嵌 OSC 8"* 只返回纯文本 -- 链接不可点击。本演示对比"可点击链接" vs "acecode 的纯色文本"。

### 03_kitty_keyboard.py
**kitty keyboard 协议**:启用后 Shift+Enter 报 `\x1b[13;2u` 而非裸 `\r`,修饰键/释放事件都可区分。交互式捕获按键并显示原始字节。opencode/pi/grok 启用;acecode 未启用(仅消费 `\x1B[1;3A` 等标准序列)。**注意:Windows Terminal 目前不支持 kitty 协议**,在 WT 上会回退到标准序列(这本身就是演示的一部分)。

### 04_alpha_transparency.py
**RGBA alpha 混色**:ANSI 无法逐 cell 设 alpha,opencode/grok 在 framebuffer 层做 per-pixel 合成(半透明遮罩、透出终端背景的 system 主题)。本演示用**预混色**模拟视觉效果:把叠层颜色与背景色按 alpha 混合后输出,对比 acecode 的不透明色块。

### 05_animated_background.py
**正弦呼吸动画背景**:`sin²(t)` 驱动背景亮度循环,模拟 opencode `BgPulse`(呼吸光圈)与 grok `wave_brightness`(跨行相位波)。acecode 无背景动画。

### 06_spinner_showcase.py
**五种 spinner 同屏**:acecode `●`(静态点)、grok braille `⠋⠙⠹⠸⠼⠴⠦⠧`、grok dot `⋅ : ⸬ ⁙`、grok monitor `○ ◎ ◉ ◎`、opencode Knight Rider 扫描光带(逐像素 alpha 渐变拖尾)。

### 07_osc133_prompts.py
**OSC 133 prompt 语义标记**:`\x1b]133;A\x07`(prompt 开始)/ `B`(输出开始)/ `C`(输出结束)。支持的终端(kitty/WezTerm)可 Cmd+Shift+↑/↓ 在 prompt 间跳转。pi 用此做 transcript 导航;acecode 无。

### 08_tool_row_dots.py
**acecode 独有优势**:` ● ToolName(args)` 三态指示灯(灰=执行中/绿=成功/红=失败),`compute_tool_call_dots` FIFO 配对 tool_call ↔ tool_result。演示 acecode 的紧凑 transcript 风格,与其他项目的纯文本/accent 竖线对比。

### 09_streaming_markdown.py
**流式 markdown 增量渲染基准**:模拟流式逐 chunk 追加文档,对比三种方法的耗时:
- 朴素法(acecode):每 chunk 整条重渲染
- stable-prefix(crush):缓存安全前缀,只渲染尾部
- checkpoint 冻结(grok):冻结已稳定块,只渲染活跃 tail

展示 acecode 在长输出时的 CPU 开销差距。
