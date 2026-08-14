# ACECode TUI 与同类项目对比调研报告

> 调研日期:2026-08-13
> 调研范围:`D:\dev` 下 5 个具备终端 TUI 的 AI 编程工具:
> **acecode**(本项目,C++ + FTXUI)、**opencode**(TS + OpenTUI)、**pi**(TS + 自研 pi-tui)、**crush**(Go + Bubble Tea v2)、**grok-build**(Rust + ratatui)。
> `agent`(纯笔记)、`openchamber` / `pi-web`(web/desktop,无 TUI)不参与对比。
>
> 配套可运行效果演示见 [`demos/`](./demos/README.md)。

---

## 一、技术栈速览

| 项目 | 语言 | TUI 框架 | 渲染模型 | Markdown / 语法高亮 |
|---|---|---|---|---|
| **acecode** | C++ | **FTXUI** | 立即模式组件树,每帧重算 → Screen → ANSI | 自研 `markdown_formatter` + 内置 `syntax_highlight` |
| **opencode** | TS/bun | **OpenTUI**(`@opentui/core`+`solid`) | retained framebuffer(`Uint16Array` 逐像素)+ Yoga flexbox + 原生 FFI | OpenTUI `<markdown>` + **tree-sitter WASM 运行时下载** |
| **pi** | TS | **自研 pi-tui** | 组件树每帧渲染成文本行数组 → **逐行字符串 diff** → CSI 2026 同步输出 | `marked` + highlight.js + LaTeX Unicode |
| **crush** | Go | **Bubble Tea v2** + lipgloss | 命令/更新/视图,`View()` 返回整串,tea 内部行 diff | **glamour v2 + stable-prefix 增量缓存** |
| **grok-build** | Rust | **ratatui** + crossterm | 立即模式 + 双缓冲 **cell 级 diff** | 自研 `xai-grok-markdown` + checkpoint 冻结 + syntect |

---

## 二、本项目(acecode)TUI 实现特征

- **立即模式 + FTXUI 组件树**:`make_screen_interactive`(`src/tui/render_mode.cpp`)按 `tui.alt_screen_mode`(auto/always/never)在 `Fullscreen()`(alt-screen `\033[?1049h`)与 `TerminalOutput()` 间二选一;`run_tui_loop`(`src/tui/tui_init.cpp:296`)直接 `screen.Loop(renderer)`,bracketed paste 包裹。
- **布局**:header(ACE block-art logo + 版本/模型/cwd)+ 右侧 sidebar(会话列表)+ chat transcript + input box + status line。
- **Markdown**:自研 `format_markdown`(`src/markdown/markdown_formatter.cpp`)输出 FTXUI `Element`,有 `syntax_highlight`(按语言名内置表,`src/markdown/syntax_highlight.cpp`),theme 有 `MarkdownColors`(code_span/link/bold/heading/block_code…)。**流式时整条重渲染**,靠 `redraw_pacer`(`src/tui/redraw_pacer.cpp`)帧率限制 + `tool_result_fold` 折叠控量。
- **工具行渲染**:`● ToolName(args)` 三态指示灯(灰=执行中 / 绿=成功 / 红=失败)、`compute_tool_call_dots` FIFO 配对、`pascal_case_tool_name`、Ctrl+O 全局展开 / Ctrl+E 逐行、`thinking_heartbeat` 内联 `[Ns · ↓ X tokens]`。
- **跨平台兜底**:`detect_terminal_capabilities` 读 ConEmuPID/WT_SESSION/Windows build,`should_use_conhost_compat_layout` 在老 conhost 上退 ASCII 边框、去 logo —— 本项目相对其他项目**最深入的老 Windows 终端兼容**。
- **剪贴板/鼠标**:FTXUI mouse tracking + 右键 OSC 52 复制 + Ctrl+V 读系统剪贴板(`src/utils/clipboard.*`)。

---

## 三、关键差异分维度对比

### 3.1 渲染管线(差异最大)

| | 模型 | 同步输出 | alpha 透明 |
|---|---|---|---|
| **acecode** | FTXUI 立即模式,Screen diff,光标回退 `\033[1A` | ❌ 无 | ❌ FTXUI 颜色不透明 |
| **opencode** | retained framebuffer 逐像素 + Yoga flexbox | (OpenTUI 原生层) | ✅ RGBA alpha 混色 |
| **pi** | 文本行数组 + 逐行字符串 diff | ✅ CSI 2026 | ❌(整行不透明) |
| **crush** | Bubble Tea `View()` 返回整串,tea 行 diff | (Bubble Tea 内部) | ❌ |
| **grok-build** | ratatui 双缓冲 cell diff + 后台写线程 | ✅ CSI 2026 | ✅(framebuffer 合成) |

- **grok 独有**:改写 ratatui draw 循环以**保留光标闪烁**(去重 `Show`/`MoveTo`),渲染到 **stderr** + **后台写线程**解耦事件循环与 pty 背压;acecode 无此层。
- **pi 独有**:`TuiMainScreen`(主缓冲 + 终端 scrollback)与 `TuiAltScreen`(固定视口 + 应用自持滚动)双模式,逐行 diff + `maxLinesRendered` 高水位。
- **acecode**:FTXUI 默认刷新无同步输出协议,老终端上 banner 堆叠靠 alt-screen 兜底(CLAUDE.md 记录的 `\033[?1049h` workaround)。

### 3.2 流式 Markdown 渲染(acecode 明显偏弱)

| | 做法 |
|---|---|
| acecode | 整条重渲染 + `redraw_pacer` 限帧 |
| **crush** | glamour "stable-prefix" 增量缓存 —— 只重渲染安全边界之后的尾部(`internal/ui/chat/streaming_markdown.go`) |
| **grok** | `StreamingMarkdownRenderer` checkpoint 冻结 tail,`(width,generation,theme)` 键控换行缓存(`xai-grok-markdown/src/streaming.rs`) |
| **opencode** | OpenTUI `streaming={true}` 增量解析 + SSE 16ms 批量合并成单次渲染 |
| **pi** | 整条重解析但 markdown `cachedText/cachedWidth` 缓存 + `trimPartialClosingFences` 防抖 |

→ acecode 在长 markdown 流式时 CPU 开销最高(无增量、无缓存层)。见 [`demos/09_streaming_markdown.py`](./demos/09_streaming_markdown.py)。

### 3.3 代码语法高亮

| | 方案 | 依赖 |
|---|---|---|
| acecode | 内置语言表 `syntax_highlight.hpp` | **离线、零下载** |
| **opencode** | **运行时从 GitHub 下载 tree-sitter WASM + nvim-treesitter 高亮查询**(约 30 语言) | 网络依赖,能力最强 |
| grok | syntect + 内置 tmTheme,按终端色彩能力 quantize,16 色下双极性安全降级 | 离线 |
| pi | highlight.js,`supportsLanguage` 为真才高亮否则整块单色 | 离线 |

### 3.4 视觉特效(acecode 基本没有)

| 效果 | acecode | opencode | pi | grok |
|---|---|---|---|---|
| 逐帧动画背景 | ❌ | ✅ `BgPulse` 正弦呼吸光圈 + GO logo 高光扫动 | ❌ | ✅ `wave_brightness` sin² 波 + `pulse_brightness` |
| RGBA 透明/混色 | ❌ | ✅ 半透明对话框遮罩、透出终端的 system 主题 | ❌ | ✅ framebuffer 合成 |
| Knight Rider 扫描 spinner | ❌ | ✅ 逐像素 alpha 渐变拖尾 | ❌ | ❌ |
| braille/dot/monitor spinner | ❌ | ❌ | ✅ loader 盲文帧 | ✅ 三套帧集 |
| 内联图片 | ❌ | ❌ | ✅ Kitty/iTerm2 像素图 | ✅ Kitty/iTerm2 |
| 内联视频 | ❌ | ❌ | ❌ | ✅ ffmpeg 抽帧 |
| Mermaid 终端渲染 | ❌ | ❌ | ❌ | ✅ Unicode box-drawing |
| 工具行 `●` 三态指示灯 | ✅(独有风格) | 文本 | 文本 | accent 竖线 |

→ 见 [`demos/05_animated_background.py`](./demos/05_animated_background.py)、[`demos/06_spinner_showcase.py`](./demos/06_spinner_showcase.py)、[`demos/04_alpha_transparency.py`](./demos/04_alpha_transparency.py)、[`demos/08_tool_row_dots.py`](./demos/08_tool_row_dots.py)。

### 3.5 输入编辑器

| | 能力 |
|---|---|
| acecode | `input_history_navigation`、`path_reference_input`、`paste_handler`、`ime_windows`;**无 extmark、无输入内高亮、无 leader key** |
| **opencode** | OpenTUI `TextareaRenderable` —— minHeight/maxHeight 自增高、**输入框内语法高亮**、**extmark 虚拟文本**(Neovim 式 `[Image 1]`/`[Pasted ~N lines]`/agent 标签)、IME 组合、shell `!` 前缀、keymap mode stack + **leader key + chord** |
| **pi** | 自研 editor —— Emacs 风格移动、undo stack、kill-ring、slash/路径自动补全、bracketed paste 大粘贴压缩、CJK 断行(`Intl.Segmenter`)、**IME APC 光标定位** |
| **grok** | `xai-ratatui-textarea` grapheme 编辑 + Vim/Simple 双模式 + 圆角 PromptWidget |

### 3.6 终端协议(acecode 的核心差距)

| 协议 | acecode | opencode | pi | crush | grok |
|---|---|---|---|---|---|
| **CSI 2026 同步输出** | ❌ | (原生层) | ✅ | (tea 内部) | ✅ |
| **OSC 8 超链接** | ⚠️ 有检测代码但**不发射**(FTXUI Element 无法内嵌,`make_hyperlink` 返回纯文本) | ✅ | ✅ | ❌ | ✅ 深度集成(路径/URL 自动 linkify) |
| **OSC 133 prompt 标记** | ❌ | ❌ | ✅ | ❌ | ❌ |
| **kitty keyboard 协议** | ❌(仅消费标准 CSI 修饰符,未显式启用) | ✅ | ✅ | ❌ | ✅ |
| **modifyOtherKeys** | ⚠️ 消费 `\x1B[1;3A` 等标准序列,未显式启用 | (OpenTUI) | ⚠️ 回退方案 | (tea) | (crossterm) |

**acecode OSC 8 现状**(已核实源码):`src/markdown/markdown_formatter.cpp:78` 有 `terminal_supports_hyperlinks()` 检测 WT_SESSION/TERM_PROGRAM(iTerm/WezTerm/vscode/xterm),但 `make_hyperlink()` 注释明确写 *"This only works at the terminal level, not in FTXUI Elements. We'll just return the display text since FTXUI handles rendering."* —— 即检测到支持也只渲染纯色文本,**链接不可点击**。这是 FTXUI Element 网格模型的架构限制,非简单 bug。

→ 见 [`demos/01_synchronized_output.py`](./demos/01_synchronized_output.py)、[`demos/02_osc8_hyperlinks.py`](./demos/02_osc8_hyperlinks.py)、[`demos/03_kitty_keyboard.py`](./demos/03_kitty_keyboard.py)、[`demos/07_osc133_prompts.py`](./demos/07_osc133_prompts.py)。

### 3.7 跨平台 / 终端处理

| | Windows 处理 | 终端探测 |
|---|---|---|
| acecode | conhost compat 布局(ASCII 边框去 logo)+ alt-screen 兜底;**无 UTF-8 CP 强制、无 VTP 显式开启** | ConEmuPID/WT_SESSION/Windows build |
| **grok** | `SetConsoleMode` 给 **stderr** 开 VTP + `SetConsoleOutputCP(CP_UTF8)`(否则盲文/box-drawing 乱码)、legacy ConHost 字形回退(`❯->>`、spinner->ASCII)、QuickEdit 原生选择保留 | DA2/XTVERSION 探测 **20+ 终端品牌** |
| **pi** | 原生 `win32-console-mode.node` 开 `ENABLE_VIRTUAL_TERMINAL_INPUT` + `isModifierPressed`(GetAsyncKeyState) | kitty keyboard 协商 + modifyOtherKeys 回退 |
| opencode | kernel32 FFI 清 `ENABLE_PROCESSED_INPUT` + 每 100ms 轮询 Ctrl+C guard | OpenTUI 原生层 |
| crush | Bubble Tea 自带 + catwalk 测试录制 | (tea 内部) |

acecode 的 conhost compat 布局是**独门老终端兜底**,其他项目大多只测 Windows Terminal;但 acecode **没有 kitty keyboard、没有 UTF-8 CP 强制、没有终端品牌探测**。

### 3.8 主题系统

| | 方案 |
|---|---|
| acecode | dark/light 两套预置 `ThemePalette`(Ui/Diff/Syntax/Markdown/Semantic),不透明色 |
| **opencode** | 30+ JSON 主题 + 用户自定义 + **system 主题从终端调色板现场生成** + 亮暗自动探测 |
| **grok** | GrokNight/GrokDay/TokyoNight/RosePineMoon/OscuraMidnight/Auto + OSC 11 跟随终端背景 + 颜色能力量化降级 |
| **pi** | 60+ token、256/truecolor 自动切换、文件 watcher 热重载 |

### 3.9 虚拟化 / 性能

| | 手段 |
|---|---|
| acecode | `redraw_pacer` 帧率限制 + `tool_result_fold` + `chat_render_window` |
| **grok** | `entry_layouts_cache` 条目高度缓存 + scrollback 虚拟化 + `SafeBuf` 防 resize 竞态越界 panic |
| **pi** | per-frame `renderCache` + render-churn bench 基准 |
| **opencode** | ScrollBoxRenderable 裁剪 + SSE 16ms batching + conceal 代码隐藏 |
| crush | stable-prefix glamour 缓存 + assistant_section_cache + prefix_cache |

---

## 四、acecode 独有 / 仍占优的点

1. **单一 C++ 二进制、零运行时依赖** —— opencode 要下 tree-sitter WASM、pi 要装 node ≥22 + 原生 addon、grok 编 Rust。
2. **老 Windows 终端(conhost/Cmder/ConEmu)兼容最扎实** —— conhost compat 布局 + alt-screen 兜底,其他项目大多只测 Windows Terminal。
3. **`●` 工具调用行 + Ctrl+O/E 折叠语义** —— 本项目自创的紧凑 transcript 风格,信息密度高且与 Claude Code 对齐。
4. **daemon + 桌面壳 + headless 多形态共用同一 TUI 渲染逻辑** —— 其他项目大多是单体 TUI。

---

## 五、acecode 最值得补的差距(按性价比排序)

| # | 差距 | 难度 | 收益 | 参考实现 |
|---|---|---|---|---|
| 1 | **流式 markdown 增量渲染** | 中 | 高(长输出卡顿最明显) | crush stable-prefix / grok checkpoint |
| 2 | **CSI 2026 synchronized output** | 低 | 中(消除闪烁) | pi/grok |
| 3 | **OSC 8 超链接真正可用** | 高(需绕过 FTXUI Element 限制) | 中 | grok linkify;或在 FTXUI 外层 post-process |
| 4 | **kitty keyboard 协议** | 中 | 中(可靠 Shift+Enter/修饰键) | opencode/pi/grok |
| 5 | **OSC 133 prompt 标记** | 低 | 低(transcript 导航) | pi |
| 6 | **主题系统扩展**(用户 JSON + 调色板生成) | 中 | 中 | opencode/pi |
| 7 | **输入框 extmark + 输入内高亮** | 高 | 中 | opencode(FTXUI 限制大) |

> 第 3、7 项受 FTXUI Element 网格模型限制较深,可能需要在 FTXUI 渲染后做 ANSI 后处理或局部绕过 FTXUI。

---

## 六、演示索引

所有演示为自包含 Python 脚本,在 Windows Terminal / kitty / WezTerm / iTerm2 下可直接运行:

```
cd docs/tui-comparison/demos
python 01_synchronized_output.py     # CSI 2026 同步输出:闪烁对比
python 02_osc8_hyperlinks.py         # OSC 8 可点击超链接
python 03_kitty_keyboard.py          # kitty keyboard 协议(交互)
python 04_alpha_transparency.py      # RGBA alpha 混色对比
python 05_animated_background.py     # 正弦呼吸动画背景
python 06_spinner_showcase.py        # 五种 spinner 同屏对比
python 07_osc133_prompts.py          # OSC 133 prompt 语义标记
python 08_tool_row_dots.py           # acecode ● 三态指示灯(本项目优势)
python 09_streaming_markdown.py      # 流式 markdown 增量渲染基准
```

详见 [`demos/README.md`](./demos/README.md)。
