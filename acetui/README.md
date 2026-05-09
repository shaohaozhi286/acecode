# acetui

`acetui` 是 acecode 仓内的一个独立 C++17 TUI 学习项目,目标是用 C++ 把
[codex](https://github.com/openai/codex) `codex-rs/tui/` 的 TUI 渲染层能力
**从零自己写一遍**(读 codex 设计 / 自己用 C++ 表达,不翻译源码),作为
acecode 后续 TUI 重构的设计参考与沙盒。

> 设计提案:`openspec/changes/add-acetui-cpp-port/proposal.md`
>
> 不属于本库范围的 capability:`mcp` / `app-server` / `protocol` / `core` /
> 各种 backend / 业务逻辑模块。`acetui` 只负责 TUI 渲染层。

## 构建

opt-in,默认 OFF。打开开关后才编:

```bash
cmake -S . -B build -DACECODE_BUILD_ACETUI=ON
cmake --build build --target hello_chat --config Release
./build/acetui/Release/hello_chat.exe   # Win
# ./build/acetui/hello_chat              # POSIX
```

不依赖任何第三方 TUI 库(没有 FTXUI / ncurses / termcap)。Win32 走
`SetConsoleMode` + `ReadConsoleInputW`,POSIX 走 `termios` + `read` +
`SIGWINCH`。

## Phase 1 已落地能力

| 模块 | 头文件 | 行为 |
|---|---|---|
| `acetui::Terminal` | `include/acetui/terminal.hpp` | raw mode + VT processing,跨平台 4 个静态接口 |
| `acetui::Event` | `include/acetui/event.hpp` | `std::variant<KeyEvent, ResizeEvent, PasteEvent, MouseEvent>` |
| `acetui::Viewport` | `include/acetui/viewport.hpp` | 1-based 矩形 + 贴底构造 |
| `acetui::insert_history_lines` | `include/acetui/insert_history.hpp` | DECSTBM 滚动区把历史推进 scrollback |
| `acetui::App` / `Widget` / `AppContext` | `include/acetui/app.hpp` | 主循环 + 纯虚 Widget 接口 + Esc/Ctrl+C 默认退出 |
| `acetui::widgets::ChatComposer` | `include/acetui/widgets/chat_composer.hpp` | 单行 input + 上下 separator + Backspace + Enter 提交 |

`examples/hello_chat.cpp` 是端到端 demo,用上面这一套 API 拼一个最小聊天
输入条:用户每按一次 Enter,buffer 内容自然出现在 widget 上方,进入终端
scrollback,鼠标滚轮、选中、复制全走终端原生。

## Phase 2-4 capability inventory(后续独立 change 各自落地)

下表是 codex `codex-rs/tui/src/` 88 个 .rs 文件 + 17 个子模块按 TUI / 业务
划分后的 TUI 层清单。每一项都将作为后续 acetui change 的目标 — 每开一个
change 就让 inventory 里的一行变成上面 "Phase 1 已落地能力" 表里的一行。

| Codex 模块(参考) | acetui 计划等价 | 范围 |
|---|---|---|
| `bottom_pane/` (含 chat_composer + 12+ overlay views) | `acetui::widgets::BottomPane` 模态栈 | phase 2 |
| `chatwidget/` + `chatwidget.rs` | `acetui::widgets::ChatWidget` 流式聊天 | phase 2 |
| `streaming/` + `markdown_stream.rs` | `acetui::Streaming` 流式文本 | phase 2 |
| `markdown.rs` + `markdown_render.rs` | `acetui::md::render` | phase 3 |
| `diff_model.rs` + `diff_render.rs` | `acetui::diff::render` | phase 3 |
| `clipboard_paste.rs` (bracketed paste) | `acetui::Terminal` paste mode | phase 2 |
| `clipboard_copy.rs` (OSC52) | `acetui::clipboard::copy` | phase 2 |
| mouse 解析(散落在 app + bottom_pane) | `acetui::mouse::parse_sgr` | phase 2 |
| `ascii_animation.rs` + `frames/` | `acetui::widgets::AsciiAnimation` | phase 4 |
| `live_wrap.rs` | `acetui::text::live_wrap` | phase 2 |
| `line_truncation.rs` | `acetui::text::truncate` | phase 2 |
| `key_hint.rs` + `keymap.rs` + `keymap_setup/` | `acetui::widgets::KeyHintFooter` + `acetui::Keymap` | phase 3 |
| `history_cell/` + `history_cell.rs` | `acetui::widgets::HistoryCell` | phase 3 |
| `exec_cell/` + `exec_command.rs` | `acetui::widgets::ExecCell` | phase 3 |
| `ide_context/` + `ide_context.rs` | `acetui::widgets::IdeContextDisplay` | phase 4 |
| `resume_picker/` | `acetui::widgets::ResumePicker` | phase 4 |
| `status/` | `acetui::widgets::StatusBar` | phase 3 |
| `notifications/` | `acetui::Notifications`(终端铃 + OSC) | phase 4 |
| `render/` | `acetui::render` 工具集 | 跨 phase |
| `tui/` 子模块 | acetui 自有渲染原语 | 跨 phase |
| `public_widgets/` | `acetui::widgets::*` 公开 widget | 跨 phase |
| `app_backtrack.rs` | App 返回栈 | phase 3 |
| `app_command.rs` | 命令系统 | phase 3 |
| `external_editor.rs` | 外部编辑器 (`$EDITOR`) | phase 4 |
| `file_search.rs` | 文件搜索 popup | phase 4 |
| `mention_codec.rs` | `@mention` 编解码 | phase 4 |
| `goal_display.rs` | 目标显示 widget | phase 4 |
| `additional_dirs.rs` | 附加目录显示 | phase 4 |
| `cwd_prompt.rs` | cwd 提示 widget | phase 4 |
| `branch_summary.rs` | 分支摘要 widget | phase 4 |
| `color.rs` | `acetui::Color` 调色板 | early |
| `auto_review_denials.rs` | 自动 review 拒绝 widget | phase 4 |
| `collaboration_modes.rs` | 协作模式选择 | phase 4 |
| `onboarding/` | onboarding wizard | phase 4 |

明确**不**在 acetui 范围(business / backend):`app_server_*` / `cli.rs` /
`model_*` / `local_chatgpt_auth.rs` / `external_agent_*` / `audio_device.rs` /
`get_git_diff.rs` / `debug_config.rs` 等。

## License / Attribution

`acetui` 的源码是从头自己写的 C++,不复制上游 codex 的 Rust 源码。模块结构
与命名借鉴 codex 是为了让"读 codex 源码学习 TUI 招式"这一目标的对照路径
清晰 — 架构思想本身不是版权材料。VT/ANSI escape 序列(DECSTBM 等)来自
ECMA-48 / DEC VT 系列标准,不是 codex 独有发明。

如果未来要把 acetui 当独立项目发布,需要补一份合规说明:声明模块设计参考
来源,但代码不复制。
