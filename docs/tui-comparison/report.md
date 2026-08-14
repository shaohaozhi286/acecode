# ACECode TUI 与同类项目对比调研报告

> 调研日期:2026-08-15(第 2 版,纳入 kimi-code / codex)
> 调研范围:`D:\dev` 下 7 个具备终端 TUI 的 AI 编程工具:
> **acecode**(本项目,C++ + FTXUI)、**opencode**(TS + OpenTUI)、**pi**(TS + 自研 pi-tui)、
> **kimi-code**(TS + @moonshot-ai/pi-tui 即 pi-tui fork)、**crush**(Go + Bubble Tea v2)、
> **grok-build**(Rust + ratatui fork)、**codex**(Rust + ratatui fork)。
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
| **kimi-code** | TS | **@moonshot-ai/pi-tui**(pi-tui 0.84.3 fork) | 同 pi(差分渲染 + 布局树 + CSI 2026) | 同 pi(marked + hljs),`highlight-theme.ts` 主题化 |
| **crush** | Go | **Bubble Tea v2** + lipgloss | 命令/更新/视图,`View()` 返回整串,tea 内部行 diff | **glamour v2 + stable-prefix 增量缓存** |
| **grok-build** | Rust | **ratatui**(自 fork `xai-ratatui-inline`)+ crossterm | 立即模式 + 双缓冲 **cell 级 diff**,保留光标闪烁 + 后台写线程 | 自研 `xai-grok-markdown` + checkpoint 冻结 + syntect |
| **codex** | Rust | **ratatui**(自 fork `custom_terminal.rs`)+ crossterm | 立即模式 + 双缓冲 diff,自研 Terminal;**scrollback-first** 输出模型 | `pulldown-cmark` + **two-face/syntect**(~250 语言,32 主题) |

> 血缘关系:`pi` ↔ `kimi-code` 共用同一 TUI 库(pi-tui);`grok-build` ↔ `codex` 都是"fork 了 ratatui Terminal"的 Rust 重写。七个项目实际只有 5 个独立技术路线。

---

## 二、本项目(acecode)TUI 实现特征

- **立即模式 + FTXUI 组件树**:`make_screen_interactive`(`src/tui/render_mode.cpp`)按 `tui.alt_screen_mode`(auto/always/never)在 `Fullscreen()`(alt-screen `\033[?1049h`)与 `TerminalOutput()`(scrollback 模式)间二选一;`run_tui_loop`(`src/tui/tui_init.cpp:296`)直接 `screen.Loop(renderer)`,bracketed paste 包裹。
- **布局**:header(ACE block-art logo + 版本/模型/cwd)+ 右侧 sidebar(会话列表)+ chat transcript + input box + status line。
- **Markdown**:自研 `format_markdown`(`src/markdown/markdown_formatter.cpp`)输出 FTXUI `Element`,有 `syntax_highlight`(`src/markdown/syntax_highlight.cpp`,按语言名内置表),theme 有 `MarkdownColors`。**流式时整条重渲染**,靠 `redraw_pacer` 帧率限制 + `tool_result_fold` 折叠控量。
- **工具行渲染**:`● ToolName(args)` 三态指示灯(灰/绿/红)、`compute_tool_call_dots` FIFO 配对、Ctrl+O/Ctrl+E 展开、`thinking_heartbeat` 内联 `[Ns · ↓ X tokens]`。
- **跨平台兜底**:`detect_terminal_capabilities` 读 ConEmuPID/WT_SESSION/Windows build,`should_use_conhost_compat_layout` 在老 conhost 上退 ASCII 边框、去 logo —— **老 Windows 终端兼容最扎实**。
- **剪贴板/鼠标**:FTXUI mouse tracking + 右键 OSC 52 复制 + Ctrl+V 读系统剪贴板。

---

## 三、关键差异分维度对比(7 项目)

### 3.1 渲染管线

| | 模型 | 同步输出 | 输出目标 | 独特机制 |
|---|---|---|---|---|
| **acecode** | FTXUI Screen diff,光标回退 `\033[1A` | ❌ | stdout | 双模式(alt-screen / TerminalOutput scrollback) |
| **opencode** | retained framebuffer + Yoga flexbox | (原生层) | 原生 FFI | 逐像素 RGBA framebuffer |
| **pi** | 文本行数组 + 逐行字符串 diff | ✅ CSI 2026 | stdout | main/alt 双渲染器,整帧原子写入 |
| **kimi-code** | 同 pi | ✅(同 pi) | stdout | 同 pi + agent swarm 应用层 |
| **crush** | Bubble Tea `View()` 行 diff | (tea 内部) | stdout | 命令/更新/视图模型 |
| **grok-build** | ratatui 双缓冲 cell diff | ✅ CSI 2026 | **stderr** + 后台写线程 | fork Terminal 保留光标闪烁 |
| **codex** | ratatui 双缓冲 cell diff(自 fork) | ✅ CSI 2026 | stdout | **scrollback-first**:已完成历史写真实终端 scrollback,视口只是底部小窗 |

**codex scrollback-first 模型**(报告新增重点):`insert_history.rs` 把已完成的对话历史用转义序列直接写进**真实终端 scrollback**,ratatui 只负责底部一个小 viewport —— 每帧 diff 量极小。写入前做 **URL 感知预换行**(URL 行保持整行以便终端识别为可点击),检测 zellij 用专用 `ZellijRaw` 写入路径,resize 时按终端 scrollback 容量回灌重建(VS Code 1000 行 / WT 9001 / WezTerm 3500 / Alacritty 10000 上限),75ms 去抖合并拖拽 resize。

> 与 acecode 对照:acecode 的 `TerminalOutput()` 模式同样是"历史进 scrollback"思路,但机制不同 —— FTXUI 用 `\033[1A` 光标回退重绘整个输出区,codex 是"写死历史 + 独立小视口"永不回退,滚动天然由终端负责,无需重绘。

### 3.2 流式 Markdown 渲染(acecode 明显偏弱)

| | 做法 |
|---|---|
| acecode | 整条重渲染 + `redraw_pacer` 限帧 |
| **crush** | glamour "stable-prefix" 增量缓存,只渲染尾部 |
| **grok** | `StreamingMarkdownRenderer` checkpoint 冻结 tail + 键控换行缓存 |
| **opencode** | OpenTUI `streaming={true}` 增量解析 + SSE 16ms 批量 |
| **pi / kimi** | 整条重解析但 `cachedText/cachedWidth` 缓存 + `trimPartialClosingFences` 防抖 |
| **codex** | **两区域流式模型**:稳定区 → 逐行"提交动画"滚入 scrollback,尾区 → active cell;`MarkdownStreamCollector` 换行门控(表格/fenced 代码未闭合不提前渲染);**表格 holdback**(pipe 表格整表保持 mutable 直到流结束,避免加行改列宽重塑历史);`AdaptiveChunkingPolicy` 按队列压力选单行/批量提交 |

→ codex 的提交动画 + 表格 holdback 是最成熟的流式体验;acecode 差距最大。见 [`demos/09_streaming_markdown.py`](./demos/09_streaming_markdown.py)。

### 3.3 代码语法高亮

| | 方案 | 覆盖 |
|---|---|---|
| acecode | 内置语言表 `syntax_highlight.hpp` | 离线,内置 |
| **opencode** | tree-sitter WASM **运行时下载** + nvim-treesitter 查询 | ~30 语言,网络依赖 |
| **grok** | syntect + 内置 tmTheme,quantize + 双极性安全降级 | 离线 |
| **pi / kimi** | highlight.js,`supportsLanguage` 门控 | 离线;kimi 有 `highlight-theme.ts` 主题化 |
| **codex** | **two-face(~250 语言)+ 32 内置 .tmTheme + 用户自定义**,`convert_style` 跳过 italic/underline;护栏 >512KB/>10000 行/单行 >4KiB 跳过高亮 | 离线,最全 |

### 3.4 视觉特效

| 效果 | acecode | opencode | pi | kimi-code | grok | codex |
|---|---|---|---|---|---|---|
| 逐帧动画背景 | ❌ | ✅ BgPulse 呼吸光圈 | ❌ | ❌ | ✅ sin² 波浪 accent | ❌ |
| RGBA 透明/混色 | ❌ | ✅ | ❌ | ❌ | ✅ framebuffer | ✅ **accent 按终端背景 alpha 混色** |
| **渐变品牌字** | ❌ | ❌ | ❌ | ✅ `gradientText`(逐字符 hex 插值,Kimi 品牌) | ❌ | ❌ |
| 特殊 spinner | ❌ | ✅ Knight Rider 扫描光带 | ✅ braille | ✅ **Moon 月相 + braille** | ✅ braille/dot/monitor | ✅ braille(标题动画) |
| 内联图片 | ❌ | ❌ | ✅ Kitty/iTerm2 | ✅ Kitty/iTerm2 + 缩略图 | ✅ Kitty/iTerm2 | ✅ **Sixel 手写编码 + ASCII pet** |
| 内联视频 | ❌ | ❌ | ❌ | ❌ | ✅ ffmpeg 抽帧 | ❌ |
| Mermaid 终端渲染 | ❌ | ❌ | ❌ | ❌ | ✅ Unicode box-drawing | ❌ |
| 终端标题动画 | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ Thinking 时 Braille 帧 |
| 彩蛋 | ❌ | ❌ | ❌ | ✅ easter-eggs/dance | ✅ gboom 游戏 | ✅ pets(Sixel) |
| 工具行 `●` 三态灯 | ✅(独有) | 文本 | 背景色块 | 背景色块 | accent 竖线 | `• ` bullet |

### 3.5 输入编辑器

| | 能力 |
|---|---|
| acecode | `input_history_navigation`/`path_reference_input`/`paste_handler`/`ime_windows`;**无 extmark、无输入内高亮、无 leader key** |
| **opencode** | TextareaRenderable:输入内语法高亮、**extmark 虚拟文本**、IME、keymap mode stack + leader key |
| **pi** | 自研 editor:Emacs 风格、undo、kill-ring、自动补全、CJK 断行、IME APC 光标 |
| **kimi-code** | `custom-editor.ts`(自研)+ `file-mention-provider`(`@` 文件补全)+ `wrapping-select-list` |
| **grok** | `xai-ratatui-textarea` grapheme 编辑 + Vim/Simple 双模式 |
| **codex** | 自研 `TextArea`(`bottom_pane/textarea.rs`,注释里点名 tui-textarea 的坑):**Vim 模式**(normal/operator/text-object)、kill buffer、`TextElement` 原子元素(mention/大粘贴占位)、掩码输入;**配置驱动 `RuntimeKeymap`**(app/chat/composer/editor/vim 多上下文 + **key chord 和弦**) |

### 3.6 终端协议

| 协议 | acecode | opencode | pi | kimi | crush | grok | codex |
|---|---|---|---|---|---|---|---|
| **CSI 2026 同步输出** | ❌ | (原生) | ✅ | ✅ | (tea) | ✅ | ✅ |
| **OSC 8 超链接** | ⚠️ 检测但不发射(FTXUI 限制) | ✅ | ✅ | ✅ | ❌ | ✅ 自动 linkify | ✅ **链接元数据与几何分离**(换行不含转义字节,长 URL 跨行合并) |
| **OSC 133 prompt 标记** | ❌ | ❌ | ✅ | ✅ | ❌ | ❌ | ❌ |
| **kitty keyboard** | ❌(仅消费标准 CSI 修饰符) | ✅ | ✅ | ✅ | ❌ | ✅ | ✅ **按终端精细调优** |
| **CSI-u 微调** | ❌ | 通用 | 通用 | 通用 | ❌ | 通用 | ✅ iTerm2/Ghostty 抑制 release(`>5u`)、kitty 保 repeat(`>7u`)、tmux 确认 `extended-keys-format` 才用 modifyOtherKeys、VS Code+WSL 禁用 |
| **终端探测** | 仅 env 检测 | 原生层 | kitty 协商 | 同 pi | (tea) | DA2/XTVERSION 20+ | ✅ **100ms 有界探测**(CPR/OSC10/11/键盘增强),驱动主题+键盘模式 |

**codex 终端探测**(新增):`terminal_probe.rs` 用 100ms 预算探测光标位置(CPR `ESC[6n`)、默认前景/背景色(OSC 10/11)、键盘增强支持,探测期间独占终端输入 —— 比 crossterm 默认 2s 超时更快更稳。探测结果驱动:UI accent 按终端背景自动调色、键盘模式按终端分支选择。

### 3.7 跨平台

| | Windows 处理 | 独有 |
|---|---|---|
| acecode | conhost compat 布局 + alt-screen 兜底 | 老 conhost 兼容最扎实 |
| **grok** | stderr 开 VTP + `SetConsoleOutputCP(UTF-8)`、legacy 字形回退、QuickEdit 保留 | 最彻底的字形/编码处理 |
| **codex** | `SetConsoleMode` 对 stdout+stderr 开 VTP;**关闭 `ENABLE_VIRTUAL_TERMINAL_INPUT` 走 Win32 input records**(每次 poll 重断言,防其它 console client 改回 VT 输入导致导航键变裸转义字节) | macOS fd2 dup2 /dev/null 守卫 stderr |
| **pi/kimi** | 原生 .node addon(ENABLE_VIRTUAL_TERMINAL_INPUT + isModifierPressed) | 原生修饰键查询 |
| opencode | kernel32 FFI 清 PROCESSED_INPUT + 100ms Ctrl+C guard | — |
| crush | Bubble Tea 自带 | — |

### 3.8 主题系统

| | 方案 |
|---|---|
| acecode | dark/light 两套预置 `ThemePalette`,不透明色 |
| **opencode** | 30+ JSON 主题 + 用户自定义 + **system 主题从终端调色板现场生成** |
| **grok** | GrokNight/Day + TokyoNight 等 5 套 + OSC 11 跟随终端背景 + 量化降级 |
| **pi** | JSON dark/light 60+ token,热重载 |
| **kimi-code** | `theme.ts` + **`custom-theme-loader`** + `gradient-text` + `detect.ts`(终端背景探测)+ `theme-selector` 对话框 |
| **codex** | 语法:two-face 32 主题 + 用户 `.tmTheme`;**UI 强调色按终端背景 alpha 自适应**(暗底 12% 白、亮底 4% 黑混合;表格分隔线 20% alpha 混色) |

### 3.9 性能 / 虚拟化

| | 手段 |
|---|---|
| acecode | `redraw_pacer` 帧率限制 + `tool_result_fold` + `chat_render_window` |
| **codex** | **scrollback-first**(历史出界,每帧 diff 极小)+ **FrameRequester/FrameScheduler actor + 120FPS 上限** + 多级缓存(history render cache / ActiveCellLayoutCache / stable_prefix_len_cache)+ resize 回灌行数上限 |
| **grok** | `entry_layouts_cache` 高度缓存 + scrollback 虚拟化 + SafeBuf |
| **pi/kimi** | per-frame renderCache + render-churn bench |
| **opencode** | ScrollBoxRenderable 裁剪 + SSE 16ms batching + conceal |
| crush | stable-prefix glamour 缓存 + section cache |

---

## 四、acecode 独有 / 仍占优的点

1. **单一 C++ 二进制、零运行时依赖**(opencode 下 tree-sitter WASM、pi/kimi 装 node+addon、Rust 项目要编译)。
2. **老 Windows 终端(conhost/Cmder/ConEmu)兼容最扎实**。
3. **`●` 工具调用行 + Ctrl+O/E 折叠语义**自创紧凑风格。
4. **daemon + 桌面壳 + headless 多形态共用同一 TUI 渲染逻辑**。
5. **双模式(alt-screen / scrollback)** 与 codex 的 alt/inline 双模式同构 —— 说明 acecode 的架构方向与最成熟的 codex 一致,差距在实现深度(同步输出、URL 感知回灌、逐终端调优)。

---

## 五、acecode 最值得补的差距(按性价比排序)

| # | 差距 | 难度 | 收益 | 参考实现 |
|---|---|---|---|---|
| 1 | **流式 markdown 增量渲染** | 中 | 高 | crush stable-prefix / grok checkpoint / codex 两区域+提交动画 |
| 2 | **CSI 2026 synchronized output** | 低 | 中 | pi/codex/grok |
| 3 | **OSC 8 超链接真正可用** | 高 | 中 | codex 几何分离 / grok linkify |
| 4 | **kitty keyboard / CSI-u 按终端调优** | 中 | 中 | codex keyboard_modes / opencode |
| 5 | **终端探测(CPR/OSC10/11,100ms)驱动主题+键盘** | 中 | 中 | codex terminal_probe |
| 6 | **主题扩展**(用户自定义 + 终端背景自适应 accent) | 中 | 中 | codex style.rs / kimi custom-theme-loader |
| 7 | **输入框 extmark / 输入内高亮 / key chord** | 高 | 中 | codex TextArea / opencode |
| 8 | **scrollback URL 感知写入 + resize 回灌行数上限** | 中 | 中 | codex insert_history / resize_reflow_cap |

> 第 3、7 项受 FTXUI Element 网格模型限制较深。第 8 项恰好是 acecode `TerminalOutput()` 模式的演进方向 —— 从"光标回退重绘"升级为"写死历史 + 独立视口"。

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
python 10_gradient_text.py           # kimi 渐变品牌字(新增)
```

详见 [`demos/README.md`](./demos/README.md)。
