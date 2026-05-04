# CLAUDE.md

Guidance for Claude Code working in this repo.

## Project Overview

ACECode is a terminal AI coding agent in C++17. FTXUI-based TUI, multi-turn conversations with tool calling via OpenAI-compatible APIs or GitHub Copilot. Also ships a daemon mode that exposes the same agent loop over HTTP/WebSocket.

## Build

**Prerequisites:** CMake >= 3.20, Ninja, vcpkg, C++17 compiler.

```bash
git submodule update --init --recursive

<vcpkg-root>/vcpkg install cpr nlohmann-json ftxui \
  --triplet <triplet> \
  --overlay-ports=$PWD/ports

cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=<triplet> \
  -DVCPKG_OVERLAY_PORTS=$PWD/ports

cmake --build build --config Release
```

Output: `build/acecode`. On Windows, libcurl >= 8.14 is required for TLS. A custom FTXUI overlay port lives in `/ports/ftxui/`.

**Tests:** `-DBUILD_TESTING=ON` (default), then
```
cmake --build build --target acecode_unit_tests && ctest --test-dir build --output-on-failure
```
Tests under `tests/` mirror `src/` layout, files end in `_test.cpp`. `tests/CMakeLists.txt` globs `*_test.cpp` automatically. The test binary links against `acecode_testable` (an OBJECT library covering every `src/*.cpp` **except** `src/tui/` and `src/markdown/`, which pull FTXUI). `main.cpp` is always excluded.

## Running

```bash
./build/acecode                    # Fresh session
./build/acecode --resume [id]      # Resume a previous session
./build/acecode configure          # Setup wizard
./build/acecode -dangerous         # Bypass permission checks
```

Config: `~/.acecode/config.json`. Sessions: `.acecode/projects/<cwd_hash>/`.

## Architecture

### Data Flow

User input → TUI event handler → `AgentLoop` → `LlmProvider::chat_stream()` → SSE parse → streaming tokens back to TUI. Tool calls flow through `PermissionManager` → `ToolExecutor` → result appended to history → next LLM turn.

### Key Components

- **`main.cpp`** — CLI parsing, terminal setup, FTXUI rendering loop, event dispatch. The TUI layout and event wiring live here.

- **`src/agent_loop.{hpp,cpp}`** — Multi-turn state machine on a worker thread, callbacks back to TUI. **Termination protocol**: a text-only assistant reply ends the loop (default of hermes-agent and claudecodehaha). `task_complete` is the OPTIONAL explicit terminator that renders a "Done: <summary>" row. `AskUserQuestion` is **NOT** a terminator — its answer flows back as a `tool_result`. Hard cap: `config.agent_loop.max_iterations` (default 50). Esc emits `[Interrupted]` and short-circuits.

- **`src/provider/`** — `LlmProvider` interface with `OpenAiCompatProvider` (OpenAI-compatible REST+SSE; supports DeepSeek/Qwen/OpenRouter reasoning via `delta.reasoning_content` and echoes it back on assistant messages — see `openspec/changes/support-deepseek-reasoning`) and `CopilotProvider` (GitHub device-flow OAuth). `ModelContextResolver` + `models_dev_registry` consult a bundled `models.dev/api.json` snapshot at `<exe_dir>/../share/acecode/models_dev/`; network refresh is opt-in via `config.models_dev.allow_network` or `/models refresh --network`.

- **`src/tool/`** — `ToolExecutor` registry with built-ins: `bash_tool`, `file_read_tool`, `file_write_tool`, `file_edit_tool`, `grep_tool`, `glob_tool`, `AskUserQuestion`, plus `web_search` (registered only when `cfg.web_search.enabled` is true; see `src/tool/web_search/`). `ToolResult` carries an optional `summary: ToolSummary` for the green/red one-line success/failure row; tools without `summary` fall back to the legacy 10-line fold. `bash_tool` streams cleaned output via `ctx.stream`, polls `ctx.abort_flag` every 10ms, head+tail-truncates output above 100KB, and accepts `stdin_inputs: string[]` (POSIX only).

- **`src/permissions.hpp`** — `PermissionManager` with three modes (`Default`, `AcceptEdits`, `Yolo`). Read-only tools auto-approve; write/exec prompts unless glob rules match.

- **`src/session/`** — `SessionManager` persists conversation as JSONL + metadata sidecar, lazily created on first message. `session_replay.{hpp,cpp}` expands canonical OpenAI roles into TUI pseudo-rows on `--resume`. `tool_metadata_codec.{hpp,cpp}` rides on `ChatMessage.metadata` under reserved subkeys `tool_summary` / `tool_hunks` so resume restores green/red summaries and color diffs. Decoders are lenient — malformed JSON returns `nullopt` so legacy sessions degrade to the gray fold instead of crashing. `session_serializer` uses an explicit field allowlist; runtime-only fields (`expanded`, `display_override`) never persist.

- **Tool-result rendering (`main.cpp`)** — Successful summarized rows render as one green line (`icon verb · object · m1 · m2 …`). Failed rows add up to 3 dimmed lines of stderr. `Ctrl+E` toggles `expanded` to fall back to the 10-line fold. `tool_call` rows prefer `display_override` (recomputed via `ToolExecutor::build_tool_call_preview` on resume, never persisted) over the raw `[Tool: X] {JSON}` form.

- **`src/tui/tool_progress.{hpp,cpp}`** — Live tool-progress renderer. While `state.tool_running`, replaces the thinking spinner with a 5-line tail + status block (tool name, command preview, elapsed seconds, cumulative bytes); the bottom status bar gets a compact `◑ bash 23s` chip. Pushed via `AgentLoop::on_tool_progress_*` callbacks; `main.cpp` throttles re-renders to ≥150ms. While waiting on the LLM, a `○ Thinking 14s ~82 tok` chip appears in the bottom bar (token segment after 3000ms; prefers exact `last_completion_tokens_authoritative`).

- **`src/tui/picker_scroll.hpp`** — Header-only viewport-scroll helper for keyboard-navigated overlays. Pure function `scroll_to_keep_visible(selected, prev_offset, visible_rows, total)` — no FTXUI dep, consumed by `acecode_testable`. Visible-row constants: `kResumePickerVisibleRows = 10`, `kRewindPickerVisibleRows = 10`, `kSlashDropdownVisibleRows = 8`. All three overlays support ArrowUp/Down + PgUp/PgDn + Home/End and render `↑ N more above` / `↓ M more below` indicators.

- **`src/tui/slash_dropdown.{hpp,cpp}`** — Autocomplete dropdown above the input while the buffer starts with `/` and contains no whitespace. Reads `CommandRegistry`, ranks by prefix > substring(name) > substring(description). Suppressed while resume picker, rewind picker, tool-confirmation, or AskUserQuestion overlay is active.

- **`src/tui_state.hpp`** — Central shared state: messages, input buffer, animation flags, overlays, tool-progress state, waiting-indicator state. Waiting fields reset on `on_busy_changed(true)` and are only meaningful while `is_waiting`.

- **`src/prompt/system_prompt.hpp`** — Builds dynamic system prompt from cwd info + tool registry descriptions.

- **`src/markdown/`** — Lexer + formatter from markdown to ANSI escapes, with code-block syntax highlighting.

- **`src/utils/`** — `logger.hpp` (file log, daemon mode rotates daily), `token_tracker.hpp`, `path_validator.hpp`, `encoding.hpp`, `uuid.hpp`, `stream_processing.hpp` (used by `bash_tool`: `strip_ansi`, `utf8_safe_boundary`, `feed_line_state`), `base64.hpp` (encoder only, used by OSC 52 clipboard), `paths.{hpp,cpp}` (process-level `RunMode` enum + `resolve_data_dir`: `User`→`~/.acecode/`, `Service`→`%PROGRAMDATA%\acecode\` / `/Library/Application Support/acecode/` / `/var/lib/acecode/`).

- **`src/auth/github_auth.hpp`** — Copilot device-flow OAuth, token persistence + refresh.

- **`src/daemon/`** — Background daemon wrapping the agent loop in HTTP/WebSocket (`openspec/changes/add-web-daemon`). Three startup modes converge on `worker.cpp::run_worker(opts, cfg)`: `acecode daemon --foreground`, `acecode daemon start` (POSIX double-fork+setsid / Windows `DETACHED_PROCESS`), `acecode service install` + `start` (Windows SCM via `service_win.cpp`, runs as LocalSystem). Shared startup: load config → `validate_can_start` (GUID mutex) → write pid/port/guid/token to `<data_dir>/run/` → `HeartbeatWriter` (writes JSON every 2s, reads `timestamp_ms` not mtime) → `SessionRegistry` + `LocalSessionClient` + `WebServer` → block on `server.run()` until SIGTERM/CTRL_BREAK or `request_worker_termination()`. `runtime_files.cpp` does atomic `.tmp + rename`; token is owner-only (POSIX `chmod 0600` / Windows DACL).

- **`src/web/`** — Crow 1.3.2 HTTP/WebSocket server on `127.0.0.1:28080` (configurable via `web.port`; port-in-use is fail-fast, no fallback). Routes: `GET /api/health`, `GET/POST/DELETE /api/sessions`, `GET /api/sessions/:id/messages?since=N`, `GET /api/skills`, `GET/PUT /api/mcp` (PUT writes config without auto-reload), `WS /ws/sessions/:id`. WS envelope: `{type, seq, timestamp_ms, payload}`. `auth.cpp::require_auth` — loopback free pass, non-loopback requires `X-ACECode-Token` or `?token=`; `preflight_bind_check` rejects non-loopback without a token and any `-dangerous + non-loopback`.

- **`src/session/` (daemon multiplexing)** — `SessionRegistry` is `unordered_map<id, unique_ptr<SessionEntry>>`; each entry owns its own `SessionManager` + `PermissionManager` + `AgentLoop` + `AsyncPrompter`. `SessionClient` is the abstract surface; `LocalSessionClient` is the same-process impl. `EventDispatcher` is the per-session pub/sub: monotonic seq, 1024-capacity ring buffer, atomic replay-then-register on subscribe so reconnect fills gaps without losing racy frames. `permission_prompter.cpp` indirects `AgentLoop`'s permission ask: `CallbackPrompter` (TUI synchronous modal) vs `AsyncPrompter` (daemon, emits `PermissionRequest`, blocks 5min on condvar with abort polling). Swap via `AgentLoop::set_permission_prompter`; absent injection preserves the legacy callback path (TUI is zero-changed by daemon work).

- **`src/skills/`** — User-authored `SKILL.md` discovered from `.acecode/skills`, `.agent/skills`, plus `skills.external_dirs`. `SkillRegistry` reads only YAML frontmatter at startup, lazy-loads body when invoked. Each skill registers as `/<skill-name>`; `skills_list` and `skill_view` tools expose them to the LLM. `/skills reload` rescans.

- **`src/memory/`** — Cross-session user memory under `~/.acecode/memory/`. `MemoryRegistry` scans `<name>.md` entries with `name`/`description`/`type` frontmatter (`type ∈ {user, feedback, project, reference}`), rewrites `MEMORY.md` index on every upsert/remove. Tools: `memory_read` (no args = full index; by type or name), `memory_write` (path-locked to memory dir even in Yolo mode; auto-approved). Commands: `/memory list|view|edit|forget|reload`, `/init` (LLM authors ACECODE.md via `file_write_tool`; static skeleton fallback when no provider configured).

- **`src/history/`** — Per-cwd persistent input history (`<project_dir>/input_history.jsonl`, independent of any session JSONL). `record_history` lambda in `main.cpp` centralises space-only and adjacent-duplicate suppression for both Normal and Shell (`!`) modes. Append-first; head-truncates via `<file>.tmp + rename` when over `config.input_history.max_entries`. Resilient load (missing → empty; malformed → skip-with-warning). `/history` lists oldest-first; `/history clear` wipes both memory and disk.

- **`src/project_instructions/`** — Loads `ACECODE.md` / `AGENT.md` / `CLAUDE.md` from `~/.acecode/` then walks HOME → cwd outer-first, picking at most one file per directory by `cfg.filenames` priority. Toggles `read_agent_md` / `read_claude_md` (no toggle for `ACECODE.md`). Caps: per-file `max_bytes`, aggregate `max_total_bytes`, walk-depth `max_depth`. Injected as `# Project Instructions` after tool descriptions.

### Threading Model

**TUI mode:**
1. **Agent worker** — `AgentLoop`, blocked on LLM HTTP streaming
2. **Auth thread** — Copilot device-flow polling (only when needed)
3. **Animation thread** — drives the thinking spinner

Callbacks post events back into the FTXUI `ScreenInteractive` event queue.

**Daemon mode:** main thread blocks in `Crow::App::run()`. Plus:
1. **HeartbeatWriter** — every `heartbeat_interval_ms` (default 2000)
2. **Term watcher** — blocks on `g_term_cv`, calls `server.stop()` on signal
3. **AgentLoop workers** — one detached thread per turn per active session, emits via `EventDispatcher`
4. **Crow internal pool** — handles HTTP + WebSocket
5. **AsyncPrompter waiters** — AgentLoop blocks on a per-session condvar; unblock posted from the Crow handler thread processing `decision`

### Mouse Input + Clipboard

FTXUI mouse tracking is enabled by default — wheel scrolls `chat_focus_index` by ±1 (same `scroll_chat()` helper as ArrowUp/Down). Some terminals intercept native click-and-drag selection; Shift+drag is the universal bypass. FTXUI's drag-select drives a built-in selection styled blue/white; right-click writes OSC 52 (`ESC ] 52 ; c ; <base64> ESC \`) using `src/utils/base64.hpp`, then flashes `Copied N bytes to clipboard` (auto-cleared ~2s via `status_line_clear_at`). Inside tmux, set `set -g set-clipboard on` or the OSC 52 is consumed.

### Legacy terminal fallback

FTXUI's default `TerminalOutput()` mode emits `\033[1A` per frame to rewind. This breaks on Win10 < 1809 conhost and Cmder/ConEmu pty wrappers (banner stacking, viewport drift). Workaround: alt-screen (`\033[?1049h`).

`detect_terminal_capabilities()` reads `ConEmuPID` / `WT_SESSION` env + Windows build via `RtlGetVersion`. `decide_render_mode(cfg.tui, caps)` is a pure function in `src/tui/render_mode.hpp` (no FTXUI dep, in `acecode_testable`). `make_screen_interactive` is the only FTXUI consumer.

| `tui.alt_screen_mode` | caps | decision |
|---|---|---|
| `"always"` | any | AltScreen |
| `"never"` | any | TerminalOutput |
| `"auto"` | `is_windows_terminal=true` | TerminalOutput (short-circuit) |
| `"auto"` | `is_conemu` or `is_legacy_conhost` | AltScreen |
| `"auto"` | empty | TerminalOutput |

CLI `--alt-screen` / `-alt-screen` forces alt-screen for this launch (no config write). One-time hint shown via `state.conversation` (not LLM context, not session JSONL) when auto-detect triggers AltScreen; idempotency tracked in `~/.acecode/state.json` `legacy_terminal_hint_shown`. Explicit choices (`always`, CLI flag) suppress the hint.

### Web UI (browser front-end)

`acecode daemon start` → `http://localhost:28080/` 浏览器界面。前端代码在 `web/`(顶层),build 期通过 `cmake/acecode_embed_assets.cmake` 扫描整个目录,把每个文件 hex 编码后写到 `${CMAKE_BINARY_DIR}/generated/static_assets_data.cpp`(单文件,字节数组 + `embedded_asset_map()` static map),链入 `acecode_testable` 与 `acecode` 二进制 — daemon 自带前端,无外部 CDN / npm 依赖。`web.static_dir` 为非空时改走 `FileSystemAssetSource`(开发模式,改文件即生效)。

### Web UI: 前端目录

栈是 **React 18 + Vite 5 + Tailwind v4**(*不是* 原生 Web Components,旧版 CLAUDE.md 描述已过时)。`vite-plugin-singlefile` 把所有 JS/CSS 内联到单个 `dist/index.html`,然后被嵌入资源管线吃掉。`pnpm install && pnpm build` 是必经一步(CI 与本地都跑)。

```
web/
├── index.html / vite.config.js / package.json
├── src/
│   ├── App.jsx / main.jsx / theme.jsx
│   ├── components/         ← 22 个 React 组件(ChatView/Sidebar/Message/ToolBlock/SidePanel/...)
│   ├── lib/                ← api / connection / markdown / format / sessionTitle / auth / diff / lang / sessionChanges / usePreference
│   └── styles/globals.css  ← Tailwind v4 entry + 自定义 CSS variables(亮/暗双主题)
├── public/vs-icons/        ← 单色 SVG 图标库
└── pnpm-lock.yaml
```

依赖:`react@18` + `markdown-it@14`(GFM 表格/任务清单/嵌套 list)+ `markdown-it-task-lists` + `highlight.js@11`(core + 12 种语言:c/cpp/js/ts/python/bash/json/diff/markdown/rust/go/yaml,语言别名 js→javascript 等在 `lib/markdown.js` 内 normalize)+ `diff2html@3`(line-by-line 模式渲染 file_edit/file_write 的 hunks)。bundle 体积 ~461KB(gzip ~156KB),嵌入二进制约 +600KB。

`lib/markdown.js` 收紧 URL scheme 白名单(只放 `http(s)` / `mailto:` / `/` / `./` / `../` / `#`),关闭 raw HTML(`html: false`),外链自动 `target=_blank rel=noreferrer`。`renderMarkdown(src) -> string` 签名稳定。

`lib/usePreference.js` 是 UI 偏好读写的统一入口(`usePreference(key, defaults, validator?)` 返回 `[value, setValue]`,setter 接受 partial 浅合并 / 函数式更新,localStorage 写失败静默吞)。当前订阅:`ace.theme`(由 `theme.jsx::ThemeProvider`)、`acecode.singleLayoutWidths.v1`(由 `App.jsx`,`{sidebar, sidePanel}` 像素宽)、`acecode.uiPrefs.v1`(由 `App.jsx`,`{view: 'single'|'grid4'|'grid9', sidePanelCollapsed: boolean}` — view 模式与 SidePanel 折叠态跨刷新持久化)。同一 key 在整个 App 内只允许一个 hook 实例订阅,避免多 useState 之间互覆盖。

SidePanel 折叠 UI:`ChatView` 把 `SidePanel` 包到 `<div class="ace-side-panel-shell" style={{width: collapsed ? 0 : sidePanelWidth}} data-collapsed={...}>`,折叠态宽度归 0 + opacity 过渡 200ms,SidePanel 仍 mount(tab/cache/preview 内部 state 保留)。SidePanel tab 行右端有 `.ace-side-panel-collapse-btn`(展开态),折叠态时 ChatView 顶部 header 内显示 `.ace-side-panel-expand-fab` 让用户重新展开。

### Web UI: HTTP / WS 协议增量

`add-web-chat-ui` change 在 `add-web-daemon` 的 14 条 Requirement 之上扩 9 项;`enhance-webui-chat-rendering` 又扩 1 个端点 + 协议字段;`add-webui-side-panel` 加 2 个文件浏览端点:

| 端点 / 消息 | 用途 |
|---|---|
| WS `question_request` / `question_answer` | AskUserQuestion 工具的双向异步通道(`AskUserQuestionPrompter` + 5min 超时 + abort_flag 50ms 轮询,模式同 `AsyncPrompter`) |
| `tool_start` / `tool_update` / `tool_end` payload 字段扩充 | `display_override`(`ToolExecutor::build_tool_call_preview`) / `is_task_complete` / `tail_lines:[5 lines]` / `current_partial` / `total_lines` / `total_bytes` / `elapsed_seconds` / `summary{icon,verb,object,metrics}` / `success` / `output`(失败前 N 行) / `hunks[]`(file_edit/file_write 的 diff,前端走 diff2html 渲染) — 实现:`src/web/tool_event_payload.{hpp,cpp}` 把序列化收口 |
| `message` payload(WS + REST `GET /api/sessions/:id/messages`) 扩 `id` 字段 | user 消息走持久化 UUID(`ensure_user_message_identity`);assistant/system/tool 走 lazy `sha1(role + " " + content + " " + timestamp)` 小写 hex(实现:`src/web/message_payload.{hpp,cpp}` + `src/utils/sha1.hpp`)。前端用这个 id 做 fork |
| `POST /api/sessions/:id/fork` body `{at_message_id, title?}` → `{session_id, title, forked_from, fork_message_id}` | 把 source session 截止到 at_message_id(含此条)的前缀复制到新 session;源不动;新 session 不自动启 turn。命名 `分叉<N>:<原标题>`(N=同源 sibling+1,原标题截 50 codepoint)。继承 cwd/provider/model,**不**继承 file_checkpoints。实现:`src/web/handlers/fork_handler.{hpp,cpp}`(纯函数 compute_fork_title + find_message_index_by_id) + `SessionManager::fork_session_to_new_id` |
| `GET /api/models` / `POST /api/sessions/:id/model` | 模型下拉:`saved_models` + 合成 `(legacy)` 行;v1 切的是 daemon 全局 provider(所有 session 共享一个),由 `swap_provider_if_needed` 实施 |
| `GET /api/history?cwd=&max=` / `POST /api/history` | per-cwd 输入历史,与 TUI 共享同一份 `<cwd_hash>/input_history.jsonl`,经 `InputHistoryStore::append` atomic rename |
| `PUT /api/skills/:name` body `{enabled}` / `GET /api/skills/:name/body` | 启停切换 + 查看 SKILL.md;PUT 写 `cfg.skills.disabled` 数组并 `save_config` + `SkillRegistry::reload` |
| `GET /api/files?cwd=&path=&show_hidden=` → `[{name,path,kind,size?,modified_ms?}]` | SidePanel 文件 tab 的 lazy 文件树。`cwd` 必须 ∈ `{deps.cwd}` 白名单;`path` 走 `weakly_canonical(cwd/path)` + prefix 检查防越权。硬编码 noise 黑名单(.git/node_modules/dist/build/__pycache__/.venv/venv/target/.next/.cache)始终过滤。隐藏文件(dot 开头)默认过滤,`show_hidden=1` 透出 |
| `GET /api/files/content?cwd=&path=` → `text/plain; charset=utf-8` body | SidePanel 预览 tab 读文件原文。> 5MB → 415 `{error:"file too large",size:N}`;前 512 字节出现 `\0` → 415 `{error:"binary"}`;不存在 → 404。实现:`src/web/handlers/files_handler.{hpp,cpp}`(纯函数,17 个 unit test 覆盖路径越权 / 噪音过滤 / 排序 / 二进制嗅探) |

`SessionMeta` 增加 `forked_from` / `fork_message_id` 字段(空时省略,老 meta 文件向后兼容)。Web 上每条消息 hover 浮出 `[复制] [分叉]` actions(codex 风格);分叉成功后立刻切到新 session(同 sidebar)。

handler 实现在 `src/web/handlers/{fork,models,history,skills,files}_handler.{hpp,cpp}`(纯函数,有 unit test),路由注册在 `src/web/server.cpp`。`/static/<...>` + `/` + SPA fallback 用 `CROW_CATCHALL_ROUTE` 一举处理(Crow 1.3.2 的 `<path>` 模板路由有兼容性问题)。

**SidePanel 「变更」tab limitation**:前端纯聚合 messages 中 `tool_end.hunks`,只能反映 `file_edit` / `file_write` 工具的改动。用户授权 agent 用 `bash sed`/`awk`/`git checkout` 改的文件抓不到 — 后续 follow-up 思路:让 `bash_tool` 在执行前后 diff 一遍 cwd 把 hunks 倒灌回 `ToolResult.metadata.tool_hunks`(待评估 git/non-git 区分)。

### Desktop shell + multi-workspace

`acecode-desktop.exe`(`-DACECODE_BUILD_DESKTOP=ON` 编出)是个 webview/webview 壳,默认管 N 个 daemon 子进程 — 每 workspace 一个独立 daemon(独立 loopback 端口 + 独立 token + 独立 Job Object)。daemon 内部代码零改动:每个 daemon 进程仍只服务自己的 `current_path()`,通过 `lpCurrentDirectory` 在 `CreateProcess` 时落地。

关键模块(全在 `src/desktop/`):
- `workspace_registry.{hpp,cpp}` — 扫 `.acecode/projects/<hash>/workspace.json`(`{cwd, name}`),默认命名 = `fs::path(cwd).filename()`,行内重命名走 `set_name` 原子写。
- `daemon_pool.{hpp,cpp}` — `unordered_map<hash, Slot>`,per-key condvar 串行化同 hash 并发 activate;`stop_all` best-effort。`IDaemonSupervisor` 虚基类便于单测 mock。
- `pick_active.{hpp,cpp}` — 启动选 active workspace 的纯函数:`state.json::last_active_workspace_hash` → process cwd 的 hash → registry 第一项 → 空。
- `folder_picker_win.cpp` — `IFileOpenDialog` + `FOS_PICKFOLDERS`,COM STA。
- `web_host.{hpp,cpp}` — webview 包装,暴露 `bind/eval/init_script/native_window`,debug=true 默认开 F12 DevTools。
- `main.cpp::wWinMain` — 串起所有,quit 时写 `last_active_workspace_hash` + `pool.stop_all()`。

JS↔C++ bridge(同进程,webview `bind`,无 HTTP):
- `aceDesktop_listWorkspaces()` → `[{hash, cwd, name, daemon_state, active, port?, token?}]`
- `aceDesktop_activateWorkspace(hash)` → `{port, token}` 或 `{error}`
- `aceDesktop_renameWorkspace(hash, name)` → `{ok}` 或 `{error}`
- `aceDesktop_addWorkspace()` → `{hash, cwd, name}` 或 `null`(取消)

切换 workspace 走**整页 navigate**(对设计稿 D6"无重载切换"的合理偏离):跨 loopback 端口 fetch 受 CORS 拦截,整页 navigate 让 origin 跟着切。代价是滚动 / 弹窗状态丢失,但 input box 清空 / 消息列表替换符合 spec。后续若加 `Access-Control-Allow-Origin: http://127.0.0.1:*` 可恢复无重载。

设计文档:`docs/desktop-shell/design.md` + `docs/desktop-shell/multi-workspace.md`,change:`openspec/changes/add-desktop-multi-workspace/`。

### Proxy / 抓包

`src/network/proxy_resolver.{hpp,cpp}` (with platform-specific `_posix` / `_win` bodies) injects proxy options into all cpr call sites uniformly. Design: `openspec/changes/respect-system-proxy`.

`config.network.proxy_mode`:
- `"auto"` (default) — Windows: `WinHttpGetIEProxyConfigForCurrentUser` → HKCU registry → env. POSIX: env (`HTTPS_PROXY`/`HTTP_PROXY`/`ALL_PROXY`/`NO_PROXY`, lowercase variants).
- `"off"` — force direct, ignore system/env.
- `"manual"` — use `proxy_url` (auto-prefixes `http://` if missing).

Both `main.cpp` and `daemon/worker.cpp` call `proxy_resolver().init(cfg.network)` and print `Proxy: <url-redacted-or-direct> (<source>)` (credentials masked). `proxy_insecure_skip_verify=true` adds a red `[INSECURE]` banner and disables `cpr::ssl::VerifyPeer/VerifyHost` only on proxied requests; direct requests stay strict. Safer alternative: `proxy_ca_bundle` PEM path via `cpr::ssl::CaInfo` for trusting Fiddler/Charles roots.

`/proxy` slash command: bare = show effective state; `refresh` = reprobe; `off` / `set <url>` / `reset` = session-level override (not persisted).

`ProxyResolver::effective(target_url)` priority: session override > `mode=off` > manual `proxy_url` > auto platform path > direct. NO_PROXY filters host (suffix `.foo.com`, bare suffix, `*` wildcard, case-insensitive). SOCKS5 via `manual proxy_url = "socks5://..."` (libcurl native).

**Behavior change:** Windows users upgrading get `proxy_mode = "auto"` by default, which means ACECode now follows the system proxy. To keep the old direct-only behavior: `{"network":{"proxy_mode":"off"}}`.

### Web Search

`src/tool/web_search/` 提供 LLM 可调的 `web_search(query, limit)` 工具,默认零配置:启动时 HEAD 探一次 `duckduckgo.com`(2s 超时,过 ProxyResolver),通 → 选 DuckDuckGo backend(海外/挂梯子)、不通 → 选 必应中国 backend(国内裸连)。探测结果缓存到 `~/.acecode/state.json` 的 `web_search.region_detected`,永不自动过期。

两个 backend 都是 HTML 爬取,零外部 API key。`backend_router.cpp::search_with_fallback` 在当前 backend 出 Network 错误时自动试对侧并切 active(同时更新缓存的 region 推断),Parse / RateLimited / Disabled 直接返回不 fallback。`HttpFetchFn` / `RegionProbeFn` 注入式设计让 unit test 不打真实网络。

`/websearch` slash command:
- `/websearch` — 显示 Active backend / Config backend / Region (+ detected_at) / Registered
- `/websearch refresh` — 失效缓存 + 立即重测,输出 before/after
- `/websearch use <name>` — 本会话临时切(只接受 `duckduckgo` / `bing_cn`,`bochaai` / `tavily` 是后续 PR 占位)
- `/websearch reset` — 回到 cfg + 缓存推导的 backend

`config.web_search` 可选字段:`enabled`(默认 true)、`backend`(`auto` / `duckduckgo` / `bing_cn` / `bochaai` / `tavily`,后两个本期未实现)、`api_key`(future)、`max_results`(1..10,默认 5)、`timeout_ms`(1000..30000,默认 8000)。`enabled = false` 时工具完全不注册到 ToolExecutor,LLM 看不到。Bing CN 的 `bing.com/ck/a?u=a1<base64>` 跳转链会自动 base64 解码到真实 URL,decode 失败或非 http(s) 静默回退到原 href。Daemon 与 TUI 共享同一份 state.json 缓存。

### Config Schema

`~/.acecode/config.json`:
```json
{
  "provider": "copilot",
  "openai": {
    "base_url": "http://localhost:1234/v1",
    "api_key": "",
    "model": "local-model",
    "models_dev_provider_id": "openrouter"
  },
  "copilot": { "model": "gpt-4o" },
  "context_window": 128000,
  "max_sessions": 50,
  "skills": { "disabled": [], "external_dirs": [] },
  "memory": { "enabled": true, "max_index_bytes": 32768 },
  "project_instructions": {
    "enabled": true,
    "max_depth": 8,
    "max_bytes": 262144,
    "max_total_bytes": 1048576,
    "filenames": ["ACECODE.md", "AGENT.md", "CLAUDE.md"],
    "read_agent_md": true,
    "read_claude_md": true
  },
  "models_dev": {
    "allow_network": false,
    "user_override_path": null,
    "refresh_on_command_only": true
  },
  "input_history": { "enabled": true, "max_entries": 10 },
  "agent_loop": { "max_iterations": 50 },
  "daemon": {
    "auto_start_on_double_click": false,
    "service_name": "ACECodeDaemon",
    "heartbeat_interval_ms": 2000,
    "heartbeat_timeout_ms": 15000
  },
  "web": { "enabled": true, "bind": "127.0.0.1", "port": 28080, "static_dir": "" },
  "network": {
    "proxy_mode": "auto",
    "proxy_url": "",
    "proxy_no_proxy": "",
    "proxy_ca_bundle": "",
    "proxy_insecure_skip_verify": false
  },
  "web_search": {
    "enabled": true,
    "backend": "auto",
    "api_key": "",
    "max_results": 5,
    "timeout_ms": 8000
  },
  "tui": { "alt_screen_mode": "auto" },
  "saved_models": [
    { "name": "local-lm", "provider": "openai", "base_url": "http://localhost:1234/v1", "api_key": "x", "model": "llama-3" },
    { "name": "copilot-fast", "provider": "copilot", "model": "gpt-4o" }
  ],
  "default_model_name": "copilot-fast",
  "mcp_servers": {
    "filesystem": { "command": "npx", "args": ["-y", "@modelcontextprotocol/server-filesystem", "/path"] },
    "remote-sse": { "transport": "sse", "url": "https://mcp.example.com", "sse_endpoint": "/sse" },
    "remote-http": { "transport": "http", "url": "https://mcp.example.com", "sse_endpoint": "/mcp" }
  }
}
```

`mcp_servers` entries without `transport` default to `stdio`. `sse` = legacy two-endpoint protocol; `http` = 2025-03-26 Streamable HTTP single-endpoint (default `/mcp`).

`saved_models` is a named registry; `default_model_name` points into it. Both optional — empty falls back to legacy `provider` / `openai.*` / `copilot.*`. Each entry needs `name` (must NOT start with `(` — reserved for synthesized `(legacy)` / `(session:<id>)`), `provider`, `model`. OpenAI entries also need `base_url` + `api_key`. `load_config` rejects on duplicate names, reserved prefixes, missing fields, or dangling `default_model_name`.

### Model profile resolution

At startup and on every `--resume` / `/resume`, `src/provider/model_resolver.cpp` resolves the effective `ModelEntry` in three layers:
1. `cfg.default_model_name` if found in `saved_models`
2. `<cwd_hash>/model_override.json` if present
3. Resumed `SessionMeta.provider` + `SessionMeta.model` (resume only) — matched by tuple, not name. No match builds an ad-hoc `(session:<id>)` entry borrowing from `cfg.openai`, plus a `⚠ Resumed with ad-hoc model entry` system message.

All-empty falls back to `synth_legacy_entry(cfg)` named `(legacy)`. The picker always shows it.

`LlmProvider` is a `shared_ptr` under `provider_mu`; `AgentLoop` gets a `ProviderAccessor` lambda that snapshots the pointer at turn start, so an in-flight `chat_stream` can never dangle when the main thread swaps providers. `provider_swap.cpp::swap_provider_if_needed` reuses same-provider instances (`set_model` + `OpenAiCompatProvider::reconfigure`); cross-provider calls `create_provider_from_entry` and recomputes the context window via `resolve_model_context_window`.

### `/model` command

- `/model` — text picker over `saved_models` + `(legacy)`, current row marked `*`
- `/model <name>` — in-memory switch
- `/model --cwd <name>` — switch + persist to `<cwd_hash>/model_override.json`
- `/model --default <name>` — switch + persist to `config.json` `default_model_name`

Unknown name → error, no state change. All persisting paths run under `provider_mu` and recompute `context_window`.

## CI / Release

`.github/workflows/package.yml` builds Linux x64/arm64, Windows x64, macOS x64/arm64. Releases auto-cut on `v*` tags.
