# CLAUDE.md

Implementation memory for coding agents working in this repository. For user-facing setup and run modes, use [README.md](README.md). For stable structure, use [ARCHITECTURE.md](ARCHITECTURE.md). For contributor rules, use [AGENTS.md](AGENTS.md).

## Current Runtime Surfaces

ACECode ships one main executable with terminal TUI and daemon subcommands, plus an optional desktop shell target.

- TUI mode starts from [main.cpp](main.cpp), configures provider/tools/commands, runs the FTXUI loop, and posts worker callbacks back into the UI event queue.
- Daemon mode starts from [src/daemon/cli.cpp](src/daemon/cli.cpp), converges on `worker.cpp::run_worker`, writes runtime files, then serves [src/web/](src/web) routes and WebSocket events.
- Web UI code lives in [web/src/](web/src), builds with React 18, Vite, Tailwind v4, `markdown-it`, `highlight.js`, and `diff2html`, then gets embedded from `web/dist/` by CMake.
- Desktop mode is opt-in through `-DACECODE_BUILD_DESKTOP=ON`; [src/desktop/](src/desktop) manages workspace registry, daemon pool, webview host, tray, notifications, and bridge calls.

## Build And Verification Notes

Use the command set in [AGENTS.md](AGENTS.md) as the source of truth. Important local facts:

- `acecode_testable` is the shared object library for headless logic and unit tests.
- `acecode` links `acecode_testable` plus FTXUI and TUI/markdown sources.
- `acecode_unit_tests` is available when `BUILD_TESTING=ON`.
- `acecode-desktop` is only created when desktop building is enabled.
- Rebuild [web/](web) with `pnpm build` before configuring CMake when embedded frontend assets need to change.
- Windows builds require libcurl 8.14 or newer for TLS behavior and use UTF-8 compile options.

## Agent Loop And Tools

[src/agent_loop.cpp](src/agent_loop.cpp) is the multi-turn state machine. A text-only assistant reply ends the loop. `task_complete` is an optional explicit terminator that renders a concise completion row. `AskUserQuestion` is not a terminator; its answer returns as a tool result. `config.agent_loop.max_iterations` is the hard cap.

Core tools are registered for both TUI and daemon paths: `bash`, `file_read`, `file_write`, `file_edit`, `grep`, `glob`, `task_complete`, `AskUserQuestion`, skill tools, memory tools, optional `web_search`, and MCP tools. `ToolResult` can carry summaries and hunks so TUI/web resume can render useful compact rows instead of raw output folds.

`bash_tool` streams cleaned output, polls abort state, truncates very large output, and supports POSIX `stdin_inputs`. File tools should preserve checkpoint hooks by calling `track_file_write_before` before mutating files.

## Sessions And Persistence

[src/session/](src/session) persists canonical conversation messages as one `<session-id>.jsonl` file with one `<session-id>.meta.json` sidecar per session. Runtime-only display fields are not serialized. Resume paths adopt the canonical JSONL directly and rebuild TUI pseudo-rows, tool previews, summaries, and diffs from persisted messages and metadata; they must not copy history into PID-suffixed files.

Old `<session-id>-<pid>.jsonl` / `<session-id>-<pid>.meta.json` files are unsupported experimental data. The project has not shipped a compatibility guarantee for them; if detected, tell users to delete the old project session data under `~/.acecode/projects` instead of migrating it.

Canonical shared transcripts are protected by a lightweight writer lease under the project session directory. TUI and daemon session activation acquire the lease, refresh it after writes, and release it on session end/finalize. Stale leases are recoverable when the owner PID is dead or the heartbeat timestamp is old.

Rewind support uses per-user-turn checkpoints. `SessionManager::track_file_write_before` is the hook file-mutating tools call so `/rewind` can restore file state.

Daemon session multiplexing uses `SessionRegistry`. Each session entry owns its own `SessionManager`, `PermissionManager`, `AgentLoop`, async permission prompter, and question prompter. `EventDispatcher` gives each emitted event a monotonic sequence number and keeps a bounded replay ring.

## Skills, Memory, And Project Instructions

[src/skills/](src/skills) discovers `SKILL.md` files from configured global, project, and external skill directories. Skill metadata is read from YAML frontmatter at startup; full bodies are loaded lazily through skill invocation or the `skill_view` tool.

[src/memory/](src/memory) stores Markdown memory entries under `~/.acecode/memory/` and rewrites an index on upsert/remove. `memory_write` is constrained to that directory even under broad permission modes.

[src/project_instructions/](src/project_instructions) loads configured project-instruction filenames from the global config directory and then from the project hierarchy, outer-first, subject to per-file and aggregate byte caps. The repository root intentionally keeps only canonical docs; do not add duplicate root instruction files for this repository.

## Daemon And Web UI

Daemon startup writes pid, port, guid, token, and heartbeat files under `<data_dir>/run/`. Runtime file writes are atomic where practical, and daemon tokens are owner-only on supported platforms.

[src/web/server.cpp](src/web/server.cpp) registers health, sessions, messages, skills, MCP, model, history, files, commands, fork, and static asset routes. WebSocket payloads use envelopes with `type`, `seq`, `timestamp_ms`, and `payload`.

Loopback requests bypass daemon token auth. Non-loopback requests require `X-ACECode-Token` or `?token=`, and non-loopback dangerous mode is rejected. Keep [docs/daemon-api.md](docs/daemon-api.md) in sync for protocol changes.

The frontend has pure helpers under [web/src/lib/](web/src/lib) with Node-based tests. Prefer adding data-shaping logic there rather than embedding it directly in components.

## Desktop Shell

The desktop shell runs a webview against workspace-local daemon processes. It does not change daemon internals; each daemon still serves one current working directory. Workspace switching currently uses whole-page navigation so browser origin follows the active loopback port.

Key modules:

- `workspace_registry`: persisted workspace list and names.
- `daemon_pool`: per-workspace daemon process management.
- `web_host`: native webview wrapper and bridge binding.
- `tray_icon_win` and `notifications_win`: Windows tray and notification integration.

Detailed behavior is in [docs/desktop-shell/multi-workspace.md](docs/desktop-shell/multi-workspace.md).

## Network, Proxy, And Web Search

[src/network/proxy_resolver.*](src/network) centralizes proxy behavior for cpr call sites. `proxy_mode=auto` follows platform/system proxy settings; `off` forces direct; `manual` uses `proxy_url`. Startup can probe proxy reachability and temporarily fall back to direct if the configured proxy is unreachable.

`/proxy` shows or changes the session-level effective proxy state without persisting changes.

[src/tool/web_search/](src/tool/web_search) provides optional HTML-backed search with backend auto-detection and fallback. `/websearch` shows status, refreshes region detection, or switches backend for the current session. If `config.web_search.enabled=false`, the tool is not registered.

## Model Profiles And Context Windows

Model resolution layers are:

1. `default_model_name` from `saved_models`.
2. Per-project model override.
3. Resumed session provider/model metadata.
4. Legacy provider config fallback.

Context windows resolve through model profile data, bundled models.dev metadata, provider defaults, and configured fallbacks. Session-facing create/resume/switch paths use `resolve_model_context_window_nonblocking`: cached or local values are returned immediately; uncached OpenAI-compatible `/models` probes run in the background and fill a process-local cache. The detailed rules live in [docs/model-context-resolution.md](docs/model-context-resolution.md).

## Config Notes

The config schema is intentionally sparse on write: defaults are omitted when possible. Notable sections are `saved_models`, `models_dev`, `skills`, `memory`, `project_instructions`, `agent_loop`, `daemon`, `web`, `network`, `web_search`, `tui`, `desktop`, and `mcp_servers`.

`mcp_servers` without `transport` default to stdio. `sse` is the legacy two-endpoint protocol. `http` is Streamable HTTP, defaulting to `/mcp` when no endpoint is provided.

## Useful Source Anchors

- [src/commands/builtin_commands.cpp](src/commands/builtin_commands.cpp): slash command registration and command help.
- [src/tool/tool_executor.cpp](src/tool/tool_executor.cpp): tool registry behavior, tool result message formatting, and compact call previews.
- [src/web/tool_event_payload.cpp](src/web/tool_event_payload.cpp): web serialization of tool progress and summaries.
- [src/web/message_payload.cpp](src/web/message_payload.cpp): REST/WS message payload identity and metadata.
- [src/session/session_serializer.cpp](src/session/session_serializer.cpp): persisted message field allowlist.
- [src/tui/render_mode.hpp](src/tui/render_mode.hpp): pure terminal render-mode decision logic.
- [src/utils/paths.cpp](src/utils/paths.cpp): user vs service data directory resolution.

## Maintenance Notes

**Daemon mode:** main thread blocks in `Crow::App::run()`. Plus:
1. **HeartbeatWriter** — every `heartbeat_interval_ms` (default 2000)
2. **Term watcher** — blocks on `g_term_cv`, calls `server.stop()` on signal
3. **AgentLoop workers** — one detached thread per turn per active session, emits via `EventDispatcher`
4. **Crow internal pool** — handles HTTP + WebSocket
5. **AsyncPrompter waiters** — AgentLoop blocks on a per-session condvar; unblock posted from the Crow handler thread processing `decision`

### Mouse Input + Clipboard

FTXUI mouse tracking is enabled by default — wheel scrolls `chat_focus_index` by ±1 (same `scroll_chat()` helper as ArrowUp/Down). Some terminals intercept native click-and-drag selection; Shift+drag is the universal bypass. FTXUI's drag-select drives a built-in selection styled blue/white; right-click with a non-empty selection writes OSC 52 (`ESC ] 52 ; c ; <base64> ESC \`) using `src/utils/base64.hpp`, then flashes `Copied N bytes to clipboard` (auto-cleared ~2s via `status_line_clear_at`). Right-click with no selection and Ctrl+V both read text from the system clipboard via `src/utils/clipboard.*` and feed the existing paste-normalization path; on Linux this requires `wl-paste`, `xclip`, or `xsel`. Inside tmux, set `set -g set-clipboard on` or the OSC 52 copy path is consumed.

### Legacy terminal fallback

FTXUI's default `TerminalOutput()` mode emits `\033[1A` per frame to rewind. This breaks on Win10 < 1809 conhost and Cmder/ConEmu pty wrappers (banner stacking, viewport drift). Workaround: alt-screen (`\033[?1049h`).

`detect_terminal_capabilities()` reads `ConEmuPID` / `WT_SESSION` env, Windows build via `RtlGetVersion`, and classic conhost signals (visible `ConsoleWindowClass`, hidden pseudoconsole + VT support). `decide_render_mode(cfg.tui, caps)` is a pure function in `src/tui/render_mode.hpp` (no FTXUI dep, in `acecode_testable`). `make_screen_interactive` is the only FTXUI consumer.

| `tui.alt_screen_mode` | caps | decision |
|---|---|---|
| `"always"` | any | AltScreen |
| `"never"` | any | TerminalOutput |
| `"auto"` | any | AltScreen |

CLI `--alt-screen` / `-alt-screen` forces alt-screen for this launch (no config write). One-time hint shown via `state.conversation` (not LLM context, not session JSONL) when auto-detect triggers AltScreen; idempotency tracked in `~/.acecode/state.json` `legacy_terminal_hint_shown`. Explicit choices (`always`, CLI flag) suppress the hint.

Classic/legacy conhost also enables a conservative TUI layout via `should_use_conhost_compat_layout(caps)`: the header drops the ACE block-art logo, version/model/cwd are left-aligned, and the outer frame uses only ASCII horizontal lines with no vertical sides or Unicode corners. Windows Terminal (`WT_SESSION`) suppresses this layout even when stdio is backed by console handles.

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

全局会话搜索面板(`add-webui-search-palette`):`Ctrl+K` / `Cmd+K`(经 `lib/useGlobalShortcut.js` 的 `matchShortcut` 判定,`window` keydown + preventDefault)或 TopBar 🔍 按钮触发 `SearchPalette`。前端**纯聚合**所有 workspace 的 sessions:`api.listAllWorkspaceSessions`(底层 `mergeAllWorkspaceSessions` 纯函数 + `Promise.allSettled`,单 workspace 失败不阻塞其它)→ `lib/searchSessions.js::rankSessions` 加权排序(title 前缀 +1000 / 子串 +500 / summary +200 / workspaceName +100 / fuzzy 兜底 +50,叠 24h/7d/30d 时间衰减 0~50)→ z-300 居中模态。键盘导航 ↑/↓/PgUp/PgDn/Home/End/Enter/Esc;选中同 workspace 直接 `setActiveRef`;跨 workspace 优先 `aceDesktop_activateWorkspace` + 整页 navigate `?open=<sid>`(App.jsx mount 时解析并 `replaceState` 抹掉 query),无 bridge 时降级直接 setActiveRef。数据 60s TTL 缓存,`session_status` / `session_status_snapshot` / `mark_session_read_ack` 任一 WS 帧到达即 invalidate。**后端零路由变更**。

排队卡片栈(`redesign-webui-queue-cards`):busy 期间提交的待发送消息**不进 transcript**,改由 `<QueueCardList>`(在 `<InputBar>` 上方)渲染成卡片堆。状态机(`lib/chatInputQueue.js`)与 `enqueueQueuedInput` / `cancelQueuedInput` / `markQueuedInput*` / `nextQueuedInput` / `completeQueuedInputForMessage` 全部不变;只是渲染分支换地方。每张卡片左侧 3px `.ace-queue-card-indicator` 色条标注状态(QUEUED 灰 / FAILED 红),右侧恒挂"取消"(close 图标),FAILED 多一个"重试"。状态↔标签映射收敛在 `lib/queueCardItem.js::buildQueueCardItem`(纯函数,Node 单测覆盖);DOM 端只是把这份结构映射到 className。`Message.jsx::UserBubble` 已剥离 `queued`/`onCancelQueued`/`onRetryQueued` props——transcript 里出现的 user 气泡一定是后端真实落库的消息。

### Web UI: HTTP / WS 协议增量

`add-web-chat-ui` change 在 `add-web-daemon` 的 14 条 Requirement 之上扩 9 项;`enhance-webui-chat-rendering` 又扩 1 个端点 + 协议字段;`add-webui-side-panel` 加 2 个文件浏览端点:

| 端点 / 消息 | 用途 |
|---|---|
| WS `question_request` / `question_answer` | AskUserQuestion 工具的双向异步通道(`AskUserQuestionPrompter` + 5min 超时 + abort_flag 50ms 轮询,模式同 `AsyncPrompter`) |
| `tool_start` / `tool_update` / `tool_end` payload 字段扩充 | `display_override`(`ToolExecutor::build_tool_call_preview`) / `is_task_complete` / `tail_lines:[5 lines]` / `current_partial` / `total_lines` / `total_bytes` / `elapsed_seconds` / `summary{icon,verb,object,metrics}` / `success` / `output`(失败前 N 行) / `hunks[]`(file_edit/file_write 的 diff,前端走 diff2html 渲染) — 实现:`src/web/tool_event_payload.{hpp,cpp}` 把序列化收口 |
| `message` payload(WS + REST `GET /api/sessions/:id/messages`) 扩 `id` 字段 | user 消息走持久化 UUID(`ensure_user_message_identity`);assistant/system/tool 走 lazy `sha1(role + " " + content + " " + timestamp)` 小写 hex(实现:`src/web/message_payload.{hpp,cpp}` + `src/utils/sha1.hpp`)。前端用这个 id 做 fork |
| `POST /api/sessions/:id/fork` body `{at_message_id, title?}` → `{session_id, title, forked_from, fork_message_id}` | 把 source session 截止到 at_message_id(含此条)的前缀复制到新 session;源不动;新 session 不自动启 turn。命名 `分叉<N>:<原标题>`(N=同源 sibling+1,原标题截 50 codepoint)。继承 cwd/provider/model,**不**继承 file_checkpoints。实现:`src/web/handlers/fork_handler.{hpp,cpp}`(纯函数 compute_fork_title + find_message_index_by_id) + `SessionManager::fork_session_to_new_id` |
| `GET /api/models` / `POST /api/sessions/:id/model` | 模型下拉:`saved_models`;每个 session 自带独立 `ProviderSlot`,切换走 `apply_model_to_session`(`src/provider/apply_model_to_session.{hpp,cpp}`)— TUI 与 daemon 共用同一份 helper |
| `POST` `/api/models` body `SavedModelDraft` / `PUT /api/models/<name>` / `DELETE /api/models/<name>` / `POST /api/config/default-model` body `{name}` | saved_models 增删改 + 默认设置。失败时 cfg 内存与磁盘保持原子(handler 持快照,save_config 抛异常即回滚)。响应永不携带 api_key 字段 |
| `GET /api/history?cwd=&max=` / `POST /api/history` | per-cwd 输入历史,与 TUI 共享同一份 `<cwd_hash>/input_history.jsonl`,经 `InputHistoryStore::append` atomic rename |
| `PUT /api/skills/:name` body `{enabled}` / `GET /api/skills/:name/body` | 启停切换 + 查看 SKILL.md;PUT 写 `cfg.skills.disabled` 数组并 `save_config` + `SkillRegistry::reload` |
| `GET /api/files?cwd=&path=&show_hidden=` → `[{name,path,kind,size?,modified_ms?}]` | SidePanel 文件 tab 的 lazy 文件树。`cwd` 必须 ∈ `{deps.cwd}` 白名单;`path` 走 `weakly_canonical(cwd/path)` + prefix 检查防越权。硬编码 noise 黑名单(.git/node_modules/dist/build/__pycache__/.venv/venv/target/.next/.cache)始终过滤。隐藏文件(dot 开头)默认过滤,`show_hidden=1` 透出 |
| `GET /api/files/content?cwd=&path=` → `text/plain; charset=utf-8` body | SidePanel 预览 tab 读文件原文。> 5MB → 415 `{error:"file too large",size:N}`;前 512 字节出现 `\0` → 415 `{error:"binary"}`;不存在 → 404。实现:`src/web/handlers/files_handler.{hpp,cpp}`(纯函数,17 个 unit test 覆盖路径越权 / 噪音过滤 / 排序 / 二进制嗅探) |
| `GET /api/commands?workspace=<hash>` → `{builtins:[{name,description}][, skills:[{name,description}]]}` | InputBar 斜杠下拉的命令清单。builtins **硬编码白名单 = init + compact**(描述与 TUI `register_builtin_commands` / `register_init_command` 对齐)。**`workspace` 参数(由 `expand-webui-skill-commands` 引入)**:缺省 → 不返回 `skills` 字段(向后兼容旧客户端);提供 → handler 用 `acecode::initialize_skill_registry(tmp, *cfg, workspace_cwd)` 临时构造一个 SkillRegistry 扫该 workspace 的项目链(`.agent/skills`、`.acecode/skills` + 全局 + external_dirs),与 daemon 全局 SkillRegistry 合并(workspace local 优先,first-wins by name),按字典序输出 skills 字段。`/init` `/compact` 在 web 端选中后只插入输入框 + chip 高亮,daemon 端**不**做特殊执行(原 add-webui-slash-commands 决策);`/<skill-name> args` 由下面新加的 expander 真展开。实现:`src/web/handlers/commands_handler.{hpp,cpp}`(纯函数 `build_commands_payload` + gtest case)|
| `POST /api/sessions/:id/messages`(行为扩展) | `expand-webui-skill-commands` 引入:在 `send_input` 之前调 `try_expand_skill_command(text, registry)`(`src/web/handlers/skill_command_expander.{hpp,cpp}`)。命中已知 skill 名(按 session 的 workspace cwd 临时 scan)→ text 被替换为 `build_skill_invocation_hint(meta, args)` 的**轻量提示**:`[SYSTEM: User invoked /<name> skill] + Description + Use skill_view(name=...) to load full SKILL.md + User's request: <args>`。**不**注入 SKILL.md body / supporting_files,LLM 第一次看到提示后主动 invoke `skill_view` tool 把 SKILL.md 拉一次进 context,后续重复同名 `/skill` 调用不再注入(避免 context 膨胀)。TUI `src/skills/skill_commands.cpp::cmd.execute` 也走同一个 `build_skill_invocation_hint`,跨端行为统一。Builtin (`/init`/`/compact`) 不在 SkillRegistry 中 → 透传走普通 user message;未知命令 (`/foobar`) 同样透传。**不**新增执行端点,**不**改 AgentLoop API |

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
- `tray_icon_win.{hpp,cpp}` / `notifications_win.{hpp,cpp}` / `tray_menu_layout.hpp` — 系统托盘 + OS 气泡通知。tray 注册 hidden message-only window,WndProc 接 `Shell_NotifyIcon` 回调消息(`NIN_BALLOONUSERCLICK` / `WM_LBUTTONUP` / `WM_LBUTTONDBLCLK` / `WM_RBUTTONUP`);通知用 `Shell_NotifyIconW(NIM_MODIFY, NIF_INFO)` piggyback 在同一图标上。`init_tray_icon` → `init_notifications(tray_hwnd)`,顺序不能反。V1 仅气泡,V2 计划接 WinRT ToastNotificationManager(需 AUMID + 开始菜单 .lnk + cppwinrt)。托盘图标走 `LoadImageW(IMAGE_ICON, SM_CXSMICON, SM_CYSMICON)` 直接挑 .ico 内 16×16 frame(`enhance-desktop-tray-menu`)。右键菜单走 Codex 风格(Pinned / Recent + More 子菜单 / 新建会话 / 打开 ACECode / 退出),layout 由 `tray_menu_layout.hpp::compute_menu_layout(payload)` 纯函数算 ID 编码 — 1..49 固定项 / 100..199 pinned / 200..299 recent 顶层 / 300..399 More 子菜单。
- 关窗(× / Alt+F4 / `aceDesktop_closeWindow`)默认隐藏到托盘(`config.desktop.close_to_tray`,默认 true);设为 false 回到旧的关窗即退出。真正退出走托盘 "退出" 菜单 → `WebHost::request_quit()` 派 `WM_USER+0x10` 绕过 close_request_handler;`web_host_close_policy.hpp` 的 `dispatch_wm_close` / `dispatch_request_quit` 是纯函数,unit test 可覆盖派发不变量。
- 全局单例(per-user):`single_instance.{hpp,cpp,_win.cpp,_posix.cpp}`。`wWinMain` 第一件事 `SingleInstance::try_acquire()`,失败 → `focus_existing_instance()` 拉前已有窗口再 exit(0)。Windows 走 `CreateMutexW(L"Local\\ACECode-Desktop-Singleton-v1")` + `RegisterWindowMessageW(L"ACECode_FocusExistingInstance_v1")` + `PostMessageW`,host_window_proc 收到该 msg 时 `ShowWindow + SetForegroundWindow`。POSIX 端 `flock` 在 `~/.acecode/run/acecode-desktop.lock` 拿独占锁(Linux/macOS 桌面壳落地时复用);窗口拉前在 POSIX 上 v1 stub 返 false,等 D-Bus / NSDistributedNotificationCenter 接入再补。
- `main.cpp::wWinMain` — 串起所有,quit 时写 `last_active_workspace_hash` + `pool.stop_all()` + `shutdown_notifications()` + `shutdown_tray_icon()`。

JS↔C++ bridge(同进程,webview `bind`,无 HTTP):
- `aceDesktop_listWorkspaces()` → `[{hash, cwd, name, daemon_state, active, port?, token?}]`
- `aceDesktop_activateWorkspace(hash)` → `{port, token}` 或 `{error}`
- `aceDesktop_renameWorkspace(hash, name)` → `{ok}` 或 `{error}`
- `aceDesktop_addWorkspace()` → `{hash, cwd, name}` 或 `null`(取消)
- `aceDesktop_notify({id, workspace_hash, session_id, title, body})` → `{ok}` — 投递 OS 系统通知(`add-desktop-attention-notifications`),前端在 `sessionTranscript.js` 命中 `question_request` / busy→idle+回合有 assistant 输出 时调用;前端 `lib/desktopNotify.js::shouldSuppress` 已根据 `health.notifications` cfg + `document.hasFocus()` + 当前 active session 做抑制规则,native 端不二次过滤。
- `aceDesktop_focusSession({workspace_hash, session_id})` → `{ok}` — 把窗口拉前 + 切 session。toast 点击在 native click_handler 里走相同路径(`SetForegroundWindow + ShowWindow(SW_RESTORE)` + `webview.eval` 调 `window.aceDesktop_focusSessionFromBridge` / `window.aceDesktop_activateAndOpenSession`,后两个由 `App.jsx` mount 时注册到 window)。
- `aceDesktop_setTrayMenu({workspace_name, pinned[], recent[]})` → `{ok}` — 前端推送 active workspace 的会话清单给 native 缓存,native 在右键托盘时渲染 Codex 风格菜单。前端 `lib/desktopTrayMenu.js::pushTrayMenu` 100ms debounce 入口,Sidebar.jsx 在 sessions / pinned / workspaceName 变化时调用;`activateWorkspace` bridge 内 navigate 之前 native 主动 `clear_tray_menu_payload()` 防残留。无 bridge 时 no-op(浏览器直连模式)。
- `window.aceDesktop_createNewSession()` — App.jsx 注册的全局函数(非 bind),托盘菜单"新建会话"通过 `host.eval()` 调用,等价于 TopBar 的 onNewSession 路径。

桌面通知抑制规则(`config.desktop.notifications`,默认四个 bool 均 true):`enabled` 总开关 / `on_question` AskUserQuestion 触发 / `on_completion` 回合完成触发 / `suppress_when_focused` 当前 session 已可见且窗口聚焦时跳过(避免对正盯屏的用户重复打扰)。配置经 `/api/health` 透传给前端(`health.notifications`),`lib/desktopNotify.js::maybeNotify` 一站式构造 payload + 抑制 + 投递。**多 workspace v1 限制**:webview 只连 active workspace 的 daemon,后台 workspace 的事件感知不到;v2 future work。

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

**Auto-fallback on unreachable proxy** (`proxy-fallback-on-unreachable`): 启动时调用 `proxy_resolver().probe_and_maybe_fallback()` 在 `init` 之后做一次同步 TCP probe(`src/network/tcp_probe.{hpp,cpp,_posix.cpp,_win.cpp}`)— 对解析出的代理 host:port 做非阻塞 connect + poll/WSAPoll,失败时设进程级 `fallback_active_`。横幅变成 `Proxy: direct (auto-fallback: <redacted-original-url> from <original-source> unreachable)`,所有 cpr 走直连。`/proxy` 输出新增 `Reachable : yes/no (<reason>)`,fallback 时多一行 `Original proxy : <url> (<source>)`。`/proxy refresh` 同时清 fallback + 重探(用户启动 Fiddler 后立即生效)。两个新配置:`network.proxy_probe_enabled`(默认 true,false = 一键回到旧行为)、`network.proxy_probe_timeout_ms`(默认 1500,clamp 到 [200, 10000])。Session override (`/proxy off` / `/proxy set`) 永远胜过 fallback — 用户显式意志不被二次猜测。

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
    "proxy_insecure_skip_verify": false,
    "proxy_probe_enabled": true,
    "proxy_probe_timeout_ms": 1500
  },
  "web_search": {
    "enabled": true,
    "backend": "auto",
    "api_key": "",
    "max_results": 5,
    "timeout_ms": 8000
  },
  "tui": { "alt_screen_mode": "auto" },
  "desktop": {
    "notifications": {
      "enabled": true,
      "on_question": true,
      "on_completion": true,
      "suppress_when_focused": true
    }
  },
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

`saved_models` is a named registry; `default_model_name` points into it. Each entry needs `name` (must NOT start with `(` — reserved for synthesized `(session:<id>)`), `provider`, `model`. OpenAI entries also need `base_url` + `api_key`. `load_config` rejects on duplicate names, reserved prefixes, missing fields, or dangling `default_model_name`.
`acecode configure` upserts one named `saved_models` entry from the selected provider/model and sets it as `default_model_name`; normal startup no longer derives a selectable model from top-level provider fields.

### Model profile resolution

At startup and on every `--resume` / `/resume`, `src/provider/model_resolver.cpp` resolves the effective `ModelEntry` in three layers:
1. `cfg.default_model_name` if found in `saved_models`
2. `<cwd_hash>/model_override.json` if present
3. Resumed `SessionMeta.provider` + `SessionMeta.model` (resume only) — matched by tuple, not name. No match builds an ad-hoc `(session:<id>)` entry borrowing from `cfg.openai`, plus a resumed-with-ad-hoc-model system message.

If no saved model is configured, normal startup/session creation fails instead of synthesizing a fallback model.

`LlmProvider` 由 `SessionEntry::ProviderSlot`(`shared_ptr<LlmProvider>` + `mutex`)持有。TUI 单 session 在 `main.cpp` 持一个进程级 `ProviderSlot`;daemon 每个 SessionEntry 自带独立 slot。`AgentLoop` 通过 `ProviderAccessor` lambda 在 turn 开始时拿 shared_ptr 快照,在 swap 后旧实例由快照保活到 turn 结束。切换统一走 `src/provider/apply_model_to_session.cpp`(纯逻辑,进 `acecode_testable` 单测)— 该 helper 总是 `create_provider_from_entry` 重建实例,Copilot 路径附 `try_silent_auth` 静默登录(失败仅写 `result.warning`,不抛),非致命的 meta 写盘失败同样降级为 warning。daemon 的 `SessionRegistry::switch_model` 与 TUI 的 `/model` 命令都调它。

### `/model` command

- `/model` — text picker over `saved_models`, current row marked `*`
- `/model <name>` — in-memory switch
- `/model --cwd <name>` — switch + persist to `<cwd_hash>/model_override.json`
- `/model --default <name>` — switch + persist to `config.json` `default_model_name`

Unknown name → error, no state change. All persisting paths run under `provider_mu` and recompute `context_window`.

## CI / Release

`.github/workflows/package.yml` builds Linux x64/arm64, Windows x64, macOS x64/arm64. Releases auto-cut on `v*` tags.
