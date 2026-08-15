# 终端界面对比演示

配合 [`../report.md`](../report.md) 的可运行演示。每个脚本演示一个终端效果,并标注"哪些产品有、我们现状如何"。**所有脚本都尽量用大白话描述,直接跑就能看出效果差异。**

## 运行环境

- Python 3.8+(Windows 自带 3.12 可用)
- 推荐终端:**Windows Terminal** / kitty / WezTerm / iTerm2 / Ghostty
- 老版 CMD / 老式控制台:部分效果(彩色、特殊符号)会变丑,属正常降级

## 怎么跑

```bash
cd docs/tui-comparison/demos
python 01_synchronized_output.py    # 同步刷新 vs 闪烁对比
python 02_osc8_hyperlinks.py        # 可点击链接
python 03_kitty_keyboard.py         # 键盘增强协议(交互,按 q 退出)
python 04_alpha_transparency.py     # 半透明 vs 不透明
python 05_animated_background.py    # 动态呼吸背景(动画,Ctrl+C 退出)
python 06_spinner_showcase.py       # 各家加载动画同屏(动画,Ctrl+C 退出)
python 07_osc133_prompts.py         # 回合分界标记
python 08_tool_row_dots.py          # 我们的 ● 三态指示灯(招牌)
python 09_streaming_markdown.py     # 流式增量排版 vs 整篇重排
python 10_gradient_text.py          # Kimi 渐变品牌字
```

或一键批量跑非交互的(01/02/04/07/08/09/10):

```bash
python run_static.py
```

## 每个演示讲什么

| 演示 | 演示的效果 | 谁有 | 我们现状 |
|---|---|---|---|
| `01_synchronized_output` | 同步刷新让画面一次到位不闪;不刷新的会闪 | pi / kimi / grok / codex | ❌ 没有 |
| `02_osc8_hyperlinks` | 可点击的链接(点一下打开) | opencode / pi / grok / codex | ⚠️ 写了检测但点不了 |
| `03_kitty_keyboard` | 键盘增强:分得清 Shift+Enter 和 Enter | opencode / pi / grok / codex | ❌ 没启用 |
| `04_alpha_transparency` | 半透明遮罩能透出底下文字 | opencode / grok / codex | ❌ 只能全盖住 |
| `05_animated_background` | 会"呼吸"的动态背景 | opencode / grok | ❌ 没有 |
| `06_spinner_showcase` | 各家加载动画同屏对比 | 各家都有好看的 | ❌ 仅静态 ● |
| `07_osc133_prompts` | 给每轮对话打"回合标记",支持跳转 | pi / kimi | ❌ 没有 |
| `08_tool_row_dots` | 工具调用行 ● 灰/绿/红三态灯 | 我们的招牌 | ✅ 独有 |
| `09_streaming_markdown` | 流式输出:增量排版 vs 整篇重排 | crush / grok / codex | ❌ 整篇重排 |
| `10_gradient_text` | 逐字变色的渐变字 | kimi-code | ❌ 单色 |

## 各演示一句话说明

### 01_synchronized_output.py
**同步刷新**:屏幕刷新时如果上半新下半旧,看着就闪。加了同步刷新,画面整块一次到位。先看"不加同步"的闪烁,再看"加了同步"的平滑。pi / kimi / grok / codex 都有,我们没有。

### 02_osc8_hyperlinks.py
**可点击链接**:终端里 Ctrl/Cmd+点击链接直接打开。我们其实写了"检测终端支持不支持"的代码,但因为底层框架限制,**链接最终只显示成带下划线的纯文字,点不了**。本演示对比"可点"和"我们现在的样子"。

### 03_kitty_keyboard.py
**键盘增强协议**:终端默认分不清 Shift+Enter 和 Enter(都当回车)。启用增强后能区分。本演示先看键码对照表,再进入实时捕获,按 q 退出。**注意:Windows Terminal 目前不支持这个协议**,在它上面只能看到"分不清"的效果——这本身就是我们现状的写照。

### 04_alpha_transparency.py
**半透明遮罩**:opencode/codex 能画"半透明"的对话框,底下文字隐约可见,有层次感。终端本身不支持半透明,它们是先把颜色算好再画(模拟效果)。我们只能全盖住或全透明。演示里对比"不透明(我们)"和"半透明(别人)",还有一个透明度呼吸动画。

### 05_animated_background.py
**动态背景**:像呼吸一样明暗起伏的背景,opencode 的"呼吸光圈"和 grok 的"波浪"风格。我们现在没有背景动画。Ctrl+C 退出。

### 06_spinner_showcase.py
**加载动画同屏**:把各家的加载动画摆一起转:
- 我们:静态 ●(只会亮暗变化)
- grok:盲文转圈 / 圆点 / 呼吸圆
- opencode:霓虹扫描光条(像跑马灯)
Ctrl+C 退出。

### 07_osc133_prompts.py
**回合分界标记**:给每一轮对话的起止打上隐形标记,支持的终端(kitty/WezTerm)可以一键在回合之间跳转,不用手动滚。我们现在没有,只能逐行滚。

### 08_tool_row_dots.py
**我们的招牌**:工具调用行显示成 `● ToolName(args)`,灯色代表执行状态(灰=跑 / 绿=成功 / 红=失败),结果缩进一行。还演示了"并行工具谁先完成"的配对逻辑。别人家都是纯文字或色块,这套三态灯是我们独有的。

### 09_streaming_markdown.py
**流式增量排版基准**:模拟 AI 一句一句往外冒字,对比三种做法的耗时:
- 整篇重排(我们现在):每冒一句把整篇重算,越长越慢
- 增量排版(crush):只算新冒出来的尾巴
- 冻结法(grok/codex):已稳定的段落冻结,只算在变的尾部
跑完能看到整篇重排的耗时随长度直线上升,增量方案基本不变。

### 10_gradient_text.py
**Kimi 渐变品牌字**:像品牌 Logo 那样从蓝渐变到紫的文字。Kimi 用在欢迎页和品牌元素上。我们只能整段一个颜色。演示展示了多组渐变和"渐变聚集在左/均匀分布"两种效果。
