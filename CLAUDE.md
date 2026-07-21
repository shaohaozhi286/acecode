# CLAUDE.md

Implementation m emory for coding agents working in this repository. For user-facing setup and run modes, use [README.md](README.md). For stable structure, use[ARCHITECTURE.md](ARCHITECTURE.md). For contributor rules, use [AGENTS.md](AGENTS.md).

## Project-Level Agent Overrides

The Superpowers plugin is disabled for this repository. Do not invoke or follow `superpowers:*` skills, including `superpowers:using-superpowers`, unless the user explicitly re-enables Superpowers for a specific turn.

## Current Runtime Surfaces

ACECode ships one main executable with terminal TUI and daemon subcommands, plus an optional desktop shell target.

- TUI mode starts from [main.cpp](main.cpp), configures provider/tools/commands, runs the FTXUI loop, and posts worker callbacks back into the UI event queue.
- Daemon mode starts from [src/daemon/cli.cpp](src/daemon/cli.cpp), converges on `worker.cpp::run_worker`, writes runtime files, then serves [src/web/](src/web) routes and WebSocket events.
- Headless print mode (`acecode -p "<prompt>"`, openspec add-headless-print-mode) starts from `main.cpp::dispatch_non_tui_command` → [src/headless/headless_runner.cpp](src/headless/headless_runner.cpp) — worker.cpp bootstrap 的精简复刻(无 run/ 文件、无心跳、无 Crow),会话走 SessionRegistry 正常落盘可 resume。进程级标记 `headless::active()`(src/headless/headless_mode) 有两个消费点:AgentLoop 权限门(默认自动拒绝并给模型解释文案,`--yolo` 全放行且不弹权限确认)与 AskUserQuestion(自动应答,优先于 goal unattended 分支);spawn_subagent 子会话同进程自动继承。stdout 只出最终 assistant 文本;退出码 0/1/64/130。Windows 下 -p 分支经 `CommandLineToArgvW` 重建 UTF-8 argv。**连续对话**(每轮新进程、磁盘续接,对齐 claude -p 家族一):`-c/--continue` 接当前 cwd 最近普通会话(跳过 subagent 子会话);`--resume <id>` 接指定会话 —— print 模式下 id 必填(裸 `--resume` 会把位置参数 prompt 吞成 id,与 TUI 语义刻意不同),显式给出的 `--model`/`--permission-mode` 覆盖会话保存值(`SessionRegistry::resume` 透传 + `make_entry_locked` 显式优先,web resume 不传这两个字段不受影响);`--session-id <id>` 自定新会话 id(`SessionOptions::preset_session_id`,字符集 `[A-Za-z0-9-_]{1,64}` 防路径穿越,撞已有 id 报错提示改用 --resume);`--output-format json` 成败都输出单个 `{"type":"result","session_id",...}` 对象供脚本链式调用。帮助:顶层 `acecode --help`(main.cpp `print_top_level_help`)+ `acecode -p --help`(`headless_options.cpp::print_mode_help`)。
- Headless capability policy (`openspec add-headless-tool-selection`):`-p` 新会话未传模型时走默认模型(resume 未传时沿用会话模型)、未传权限时固定 `default`、未传 `--max-turns` 时强制无限(不继承全局 cap)。工具面默认保留当前配置下实际注册的系统工具,Skill/MCP 均为空;`--disable-tools` 精确移除系统工具,`--enable-skills` 写入 runtime-only `SkillsConfig::allowed` 并只在非空时注册 `skills_list/skill_view`,`--enable-mcp` 先过滤本地 `mcp_servers` 副本再启动 runtime。三个参数都支持重复/逗号/等号形式,未知或全局 disabled 名称在 provider 前 exit 64。SessionRegistry per-entry skill registry、skill tools、slash expansion 与 spawn_subagent 都复用本次过滤后的 config/executor；allowlist 不落盘。
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

[src/agent_loop.cpp](src/agent_loop.cpp) is the multi-turn state machine. A text-only assistant reply ends the loop. `task_complete` is an optional explicit terminator that renders a concise completion row. `AskUserQuestion` is not a terminator; its answer returns as a tool result. `config.agent_loop.max_iterations` is an optional hard cap; `0` or omitted means unlimited.

`agent_loop.question_policy`(`ask` 默认 / `deny` / `timeout`,CLI `--question-policy` 覆盖只写运行时字段不落盘)控制 AskUserQuestion 应答:deny 不弹 UI 直接返回「自行决策并继续」自动应答;timeout 等 `question_timeout_seconds`(默认 60,[5,3600])秒后自动采纳每题第一选项(output/metadata 标注自动采纳)。YOLO 只跳过工具权限确认,不改变提问策略;active goal 将每次 AskUserQuestion 覆盖为 30 秒 timeout,正常弹 UI,超时采纳推荐项。daemon 通过 `AskUserQuestionPrompter::prompt` 的 per-call timeout override 实现 goal 动态覆盖。见 openspec/changes/add-ask-question-policy。

Core tools are registered for both TUI and daemon paths: `bash`, `file_read`, `file_write`, `file_edit`, `grep`, `glob`, `task_complete`, `AskUserQuestion`, skill tools, memory tools, optional `web_search`, and MCP tools. `ToolResult` can carry summaries and hunks so TUI/web resume can render useful compact rows instead of raw output folds.

`spawn_subagent` / `wait_subagent` are registered in both daemon (`worker.cpp`) and TUI (via `src/tui/subagent_host.{hpp,cpp}` — SessionRegistry/LocalSessionClient have no web dependency, so the TUI process instantiates them directly; the TUI main session lives outside the registry, so its permission mode reaches children through `SubagentToolDeps::fallback_permissions`, and `on_spawn` lets the host register tasks + subscribe child events): a sub-agent is a normal SessionRegistry session (isolated context) created with the parent's cwd and permission mode. `spawn_subagent(prompt, wait=true)` blocks until the child turn finishes and returns its final assistant reply into the parent context; `wait=false` is fire-and-forget for pipeline handoff, joined later via `wait_subagent(session_id)`. Prompts starting with `/` go through the same skill-command expansion as Web input. Sub-agents cannot spawn further sub-agents (`SessionEntry::subagent_depth`). Implementation: [src/tool/spawn_subagent_tool.cpp](src/tool/spawn_subagent_tool.cpp); deps are late-bound via shared_ptr because ToolExecutor is constructed before SessionRegistry in worker.cpp.

**TUI surface**: the right sidebar's "Background Tasks" section lists running sub-agents only (● title + elapsed; removed the moment the child turn ends — user decision); `/tasks [list|abort <id>|clear]` is the operation entry (clear = same permanent-purge semantics as Web, via `SessionStorage::purge_session_files`). A child's `permission_request` is queued (`TuiState::remote_confirm_queue`) and pumped into the confirm overlay when free (origin-labelled; the answer routes back via `SubagentHost::respond_permission`). AskUserQuestion needs no bridging — children share the TUI ToolExecutor, so the TUI ask tool runs directly; it now queues on `TuiState::overlay_cv` until the confirm/ask overlay is free and sets `ask_origin_label` when the caller is a sub-agent. `on_tool_confirm` in main.cpp queues on the same cv, so concurrent overlay claims (main confirm / child ask / remote pump) serialize instead of clobbering each other.

Full subagent reference (design, wire protocol, Web panel, TUI bridging, test map): [docs/subagents.md](docs/subagents.md).

Sub-agent sessions are **hidden from normal session lists** and surfaced only in the parent session's Web「后台任务」panel: the child meta persists `parent_session_id` (survives daemon restart; resume restores `subagent_depth=1` so the depth limit keeps holding), `GET /api/sessions` excludes them by default, `?parent=<id>` queries them, and `DELETE /api/sessions/:id?purge=1` is the panel's「清除」(destroy + delete disk data; sub-agent sessions only, busy → 409). Frontend: `web/src/lib/subagentTasks.js` (pure state, Node tests) + `lib/useSubagentTasks.js` (REST list + WS increments + retainSession for running children so their permission/question requests reach App-level listeners even with the panel closed) + `components/SubagentPanel.jsx` (overlay inside ChatView's message area: running/settled card groups, per-card stop, read-only compact transcript view reusing Message/ToolBlock with AskUserQuestion rows filtered out). A running child's `permission_request` pops the global PermissionModal and its `question_request` renders in the parent's QuestionPicker (payloads carry the child `session_id`, so answers route back automatically); both show a「来自后台任务:<title>」origin label.

`bash_tool` streams cleaned output, polls abort state, truncates very large output, and supports POSIX `stdin_inputs`. File tools should preserve checkpoint hooks by calling `track_file_write_before` before mutating files.

MCP 工具调用同样响应 abort:`McpManager::invoke` 把阻塞的 JSON-RPC 调用放进 detached 工作线程,本线程 100ms 轮询 `ctx.abort_flag`,置位立即返回 `[Aborted]`(迟到响应整体丢弃,工作线程最多活到 cpp-mcp 内部 60s 超时)。不加这层时,慢 MCP 工具(实测 windows-mcp Snapshot 30+ 秒)会让停止请求在整个调用期间失效 —— 表现为「点停止提示已终止,busy 却持续到工具自己返回」。回归测试:`tests/tool/mcp_manager_test.cpp::AbortDuringSlowToolCallReturnsQuickly`(test server `--call-delay-ms`)。

### Thread Goals(/goal,复刻 Codex ext/goal)

每 session 至多一个 goal,存项目级 `state.sqlite3`(`src/session/thread_goal_store.cpp`)。状态机:`active / paused / blocked / usage_limited / budget_limited / complete`,仅 `active` 参与自动 continuation(`AgentLoop::maybe_continue_goal`,空闲时注入 hidden `goal_context` user 消息开新回合;Plan mode 下不触发)。模型工具 `get_goal` / `create_goal` / `update_goal(complete|blocked)`;`/goal` 命令双端注册(TUI `goal_command.cpp`,daemon builtin 在 `session_registry.cpp`)。

**Goal interaction mode**:`AgentLoop::goal_unattended_active()` = 当前会话(或子代理的父会话)有 `active` goal 且非 Plan mode。为 true 时写工具权限门自动放行,AskUserQuestion 仍弹出提问组件,但固定等待 30 秒;超时后自动采纳每题第一个(推荐)选项并继续。Plan mode 只读约束优先。

**Turn error 停 goal**:provider 终止错误 / 连续空回复耗尽 / provider 缺失 → `stop_active_goal_after_turn_error`(429 → `usage_limited`,其余 → `blocked`),防止 continuation 对同一错误无限重试烧 token;`/goal resume` 恢复。用户 abort → `paused`。

**In-turn steering**:budget 耗尽转 `budget_limited` 时与 `/goal edit` 改 objective 时,置 pending 标记,worker 在下一次模型请求前(`maybe_inject_goal_steering`)注入 budget_limit wrap-up / objective_updated 提示(hidden_goal_context user 消息);回合外的 pending 标记在下一回合开始时丢弃。无 provider usage 时估算入账覆盖所有轮(含纯工具调用轮),否则 budget_limited 永不触发。

## Sessions And Persistence

[src/session/](src/session) persists canonical conversation messages as one `<session-id>.jsonl` file with one `<session-id>.meta.json` sidecar per session. Runtime-only display fields are not serialized. Resume paths adopt the canonical JSONL directly and rebuild TUI pseudo-rows, tool previews, summaries, and diffs from persisted messages and metadata; they must not copy history into PID-suffixed files.

Old `<session-id>-<pid>.jsonl` / `<session-id>-<pid>.meta.json` files are unsupported experimental data. The project has not shipped a compatibility guarantee for them; if detected, tell users to delete the old project session data under `~/.acecode/projects` instead of migrating it.

Canonical shared transcripts are protected by a lightweight writer lease under the project session directory. TUI and daemon session activation acquire the lease, refresh it after writes, and release it on session end/finalize. Stale leases are recoverable when the owner PID is dead or the heartbeat timestamp is old.

Rewind support uses per-user-turn checkpoints. `SessionManager::track_file_write_before` is the hook file-mutating tools call so `/rewind` can restore file state.

Daemon session multiplexing uses `SessionRegistry`. Each session entry owns its own `SessionManager`, `PermissionManager`, `AgentLoop`, async permission prompter, and question prompter. `EventDispatcher` gives each emitted event a monotonic sequence number and keeps a bounded replay ring.

### Worktree 隔离(EnterWorktree / ExitWorktree / --worktree,复刻 Claude Code)

worktree 落在**主仓根**的 `.acecode/worktrees/<slug>`,分支 `worktree-<flatten(slug)>`(嵌套 slug `a/b` 扁平化成 `a+b`,避免 git ref D/F 冲突与目录嵌套删除)。基线:origin/<默认分支> 本地已有直接用(大仓库不白跑 fetch),否则 fetch(`GIT_TERMINAL_PROMPT=0` 防凭据挂死),再回退当前 HEAD;`git worktree add -B` 顺带复位孤儿分支。同名 worktree fast-resume 复用。分层:[src/worktree/worktree_core.{hpp,cpp}](src/worktree/worktree_core.hpp) 纯逻辑(slug 校验/命名/PR 引用解析/.worktreeinclude gitignore 子集匹配器,无 git 依赖可单测),[src/worktree/worktree_manager.{hpp,cpp}](src/worktree/worktree_manager.hpp) git 子进程层(复用 `run_hook_process`)。

工具 `EnterWorktree` / `ExitWorktree`([src/tool/worktree_tool.cpp](src/tool/worktree_tool.cpp))经 `builtin_tool_registry` 双端注册;工具描述写死"仅用户明确提到 worktree 才可调用"。会话切换 = `AgentLoop::set_cwd`(经 `ToolContext::switch_session_cwd` 回调注入,重建 PathValidator;**会话存储位置不动** —— worktree 是同一项目的临时工作区)。状态 `WorktreeSessionInfo` 挂在 `SessionManager` 并持久化到 meta 的 `worktree_session` 字段(inactive 省略,老 meta 兼容);TUI/daemon resume 都会恢复进 worktree(目录已被外部删除则清状态)。`ExitWorktree remove` 的安全门 fail-closed:`count_worktree_changes` 数不清(git 失败/缺基线)或有未提交文件/新提交时拒绝,必须 `discard_changes:true`。

CLI `--worktree [name]` / `-w`(TUI):启动即建 worktree,name 可为 PR 引用(`#123` / GitHub PR URL → fetch `pull/N/head`,slug=`pr-N`);此时 worktree 就是项目根(日志/workspace/会话存储都在里面),与工具中途进入的 throwaway 语义不同。TUI 退出时:无变更静默删除,有变更(或数不清)保留并提示,`--resume` 恢复进去。新建 worktree 的后处理:`core.hooksPath` 指回主仓(.husky 优先)、`config.worktree.symlink_directories` symlink、`.worktreeinclude`(gitignore 语法)声明的 gitignored 文件拷贝(`ls-files --directory` 折叠目录 + 按需展开,防大仓库全量遍历)。`config.worktree.sparse_paths` 走 sparse-checkout cone 模式。**未复刻**(有意偏离):tmux 集成(POSIX-only)、WorktreeCreate/Remove hook 替换 VCS、交互式退出对话框(现为"有变更即保留"的安全默认)、spawn_subagent 的 worktree 隔离。测试:[tests/worktree/](tests/worktree/)(core 纯逻辑 + 真实 git 集成 + 工具端到端)。

## Skills, Memory, And Project Instructions

[src/skills/](src/skills) discovers `SKILL.md` files from configured global, project, and external skill directories. Skill metadata is read from YAML frontmatter at startup; full bodies are loaded lazily through skill invocation or the `skill_view` tool.

Proactive skill discovery (`inject-skill-index-into-context`): a compact skill index (name + description + optional `whenToUse` frontmatter, grouped by category) is injected each request into the session-context system-reminder built by `build_session_context_prompt` — the same per-request, never-persisted block that carries project instructions and the memory index. Budget is 1% of the context window in chars (4 chars/token, 8000-char fallback), 250 chars per entry (UTF-8-safe truncation); over budget degrades to names-only, then tail-cut with a `(+N more — call skills_list)` marker. The static `# Skills` prompt section points at this index ("err on the side of loading"); `skills_list` remains the fallback enumerator only. Without this index the model has zero visibility into installed skills and never invokes them proactively.

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

### TUI 工具行渲染(Claude Code 风格)

transcript 里的 tool_call 行渲染为 ` ● ToolName(args)`(与 user `>` / assistant `*` 行同列对齐,无箭头无额外缩进):指示灯三态 灰=执行中/无结果、绿=成功、红=失败,由 `src/tui/tool_row_format.cpp::compute_tool_call_dots` 每帧 FIFO 配对 tool_call ↔ tool_result 得出(其他角色出现即批次边界,abort 孤儿调用永远保持灰);工具名经 `pascal_case_tool_name` 转 PascalCase 加粗,颜色沿用 `syntax.preproc`;参数预览由 `parse_tool_row` 拆解(优先 display_override 的人类可读段,回退原始 JSON)。on_message push tool_call 行时就地调 `build_tool_call_preview` 填 display_override,工具执行期间即显示紧凑参数。tool_result / user_shell_output 前缀统一为缩进的 `  └ `(原 `-> ` / `<- ` 已删);结果摘要行不带 icon(工具名已在上一行加粗展示;`renderable_tool_summary_line` 在 main.cpp 与 tui_helpers.cpp 有**两份同名实现**,渲染实际走 main.cpp 的 file-static 版,两处必须同步改)。运行中进度条头同款格式(右下计时 chip 已随 inline-thinking-heartbeat 删除:等待期耗时+token 心跳内联在推理指示行右侧 `[Ns · ↓ X tokens]`(`src/tui/thinking_heartbeat.{hpp,cpp}` 纯函数,token 走回合累计语义修了多请求回合读数冻结),回合正常收尾(≥1s 且非用户中断)追加显示端伪行 `● Done for Ns`(role `turn_done`,不进 LLM context/session JSONL))。**调用/结果成对相邻**:展示派发(tool_result 伪行 + on_tool_result)从 agent_loop Phase 3 前移到各执行点 —— 并行读在收割循环里「调用行(get 之前亮出)→ 结果行」逐对派发,写工具执行完立即派发结果;Phase 3 只留 canonical 落盘(budget 替换只影响落盘,TUI 显示原始输出)。resume 回放同构:`session_replay.cpp` 把调用行攒进 pending 队列等配对结果成对推出,孤儿调用在下一条非工具消息前补推。**Ctrl+O** = 全局 verbose 开关(`TuiState::transcript_expanded`,fold 折叠 3 行 + 行尾提示 `(ctrl+o to expand)`,展开兜底上限 2000 行),与 Ctrl+E 逐行展开取或;开关位掺进 `message_render_revision`,翻转自动全量重测行高。纯逻辑单测:[tests/tui/tool_row_format_test.cpp](tests/tui/tool_row_format_test.cpp) + [tests/session/session_replay_test.cpp](tests/session/session_replay_test.cpp)。

无摘要 tool_result 的“3 行”是终端可视行而非只统计真实换行：`src/tui/tool_result_fold.cpp` 按当前结果区域宽度预切预览，真实换行和软换行都计入，因此含字面量 `\n` 的超长单行 MCP JSON 也会折叠。展开态仍以 2000 个真实行为安全上限。对应回归测试在 [tests/tui/tool_result_fold_test.cpp](tests/tui/tool_result_fold_test.cpp)。

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

**流式渲染防跳动**(fix desktop 流式出字时消息区上下跳动,参考 assistant-ui 的 block memoization):assistant 正文走 `renderMarkdownBlocks(src) -> [{key, html}]`(同文件)—— 全文 parse 一次、按 top-level token 组分别 render(共享 env,reference link 跨块有效,块 HTML 拼接与全文渲染逐字节一致,有单测不变量守护),Message.jsx 按块包 `.ace-md-block`(`display:contents`,不影响 margin 折叠/后代选择器)`dangerouslySetInnerHTML`;流式 append 时前缀块 HTML 字符串不变 → React 跳过 DOM 更新,滚动锚点不被整树替换销毁。配套:`.ace-chat-transcript-scroll` 设 `overflow-anchor:none`(浏览器原生 scroll anchoring 会跟 ChatView 的滚到底逻辑打架);`chatScrollFollow.js::isUserScrollAway` 只把「scrollHeight 稳定 + scrollTop 减小」或 userGesture(滚轮上滚 / 指针按住拖动,ChatView 挂 onWheel/onPointerDown)判为用户上滚,内容高度回落引起的 scrollTop clamp 位移不再误切 REVIEWING;highlight 结果按 `lang+源码` 缓存(128 条,满清空),已定稿代码块不重复高亮。

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
| `POST /api/open-in-explorer` body `{path}` → `{ok:true}` / 400 `{ok:false,error}` / 501 | webapp 兼容模式(Edge --app,无 webview bridge)右键菜单「在资源管理器中打开」的 REST 通路。门控 = `WebServerDeps.open_in_explorer` 回调是否注入(worker.cpp 仅在 `native_folder_picker_enabled` 时填,复用 `desktop::open_directory_in_file_manager`:绝对路径 / 目录存在 / 限已注册 workspace + daemon cwd)。前端 `DesktopContextMenu::openTargetInExplorer` bridge 优先、REST 兜底 |

`SessionMeta` 增加 `forked_from` / `fork_message_id` 字段(空时省略,老 meta 文件向后兼容)。Web 上每条消息 hover 浮出 `[复制] [分叉]` actions(codex 风格);分叉成功后立刻切到新 session(同 sidebar)。

handler 实现在 `src/web/handlers/{fork,models,history,skills,files}_handler.{hpp,cpp}`(纯函数,有 unit test),路由注册在 `src/web/server.cpp`。`/static/<...>` + `/` + SPA fallback 用 `CROW_CATCHALL_ROUTE` 一举处理(Crow 1.3.2 的 `<path>` 模板路由有兼容性问题)。

**SidePanel 「变更」tab**(redesign-sidepanel-git-changes):会话 cwd 是 git 仓库时整体切换为 **git 级视图**——`GET /api/git/changes`(numstat + name-status 按路径 join,200 条 cap + truncated 标记)列文件,单文件 diff 点击才拉(`GET /api/git/diff`,1MB cap,untracked 用 `--no-index` 合成新增 patch,diff2html 行内渲染),头部 `<branch> → <base ▾>` 只切比较基线不做 checkout(候选 = origin/<默认分支>(默认)+ HEAD)。前端缓存收口在 `web/src/lib/gitChanges.js`((cwd,base) 键 + 单文件 patch LRU 20;失效 = 回合结束 / `acecode:git-state-changed` 事件(GitSessionPill checkout 成功后广播)/ 手动刷新 / 可见且 30s 过期;面板隐藏只标脏不请求)。bash/sed/外部 IDE 的改动在 git 视图天然可见——旧「hunks 聚合抓不到 bash 改动」的 limitation 只剩**非 git 项目**(该场景保留原会话级 `lib/sessionChanges.js` 聚合视图,不变)。

**git 感知三件套**(add-git-context / add-webui-git-session-pill / redesign-sidepanel-git-changes,只抄 Claude Code 不抄 codex):`src/gitinfo/` 分 `git_context_core`(纯文本逻辑:快照拼装、2000 字符 UTF-8 截断、ref 名白名单 `is_safe_ref_name`——`.git` 文件内容可被篡改,进 prompt/REST/git argv 前必须过)+ `git_context_collector`(子进程,经 `worktree::run_git` 复用 GIT_TERMINAL_PROMPT=0,全部显式超时 `config.git_context.timeout_ms` 默认 3000ms clamp [500,30000],status/log 带 `--no-optional-locks` 不抢 IDE 的 index 锁)。三个表面:(1) gitStatus 快照按会话缓存在 `AgentLoop::git_snapshot_cache_`,首个请求惰性采集注入 session-context(`build_git_status_context_prompt`),`set_cwd`(worktree 进出)重置、`invalidate_git_snapshot()`(checkout 后,atomic 标记跨线程安全)标脏;失败/非仓库/disabled 整块静默不注入;system prompt env 段有 `Is directory a git repo: Yes|No` 行。(2) 新会话 pill(`web/src/components/GitSessionPill.jsx` + `lib/gitSessionPill.js` 纯逻辑):hero 视图与 InputBar 上方两处,分支下拉直接 checkout(409 dirty 往返弹 stash 确认,`stash push --include-untracked`;409 busy = workspace 有会话在跑,`SessionRegistry::any_busy_in_cwd` 门在任何 git 副作用之前),勾 worktree 后下拉变基线选择、首条消息经 `POST messages` 的 `worktree:{create,base}` 字段创建(`SessionRegistry::enter_worktree_for_web`,slug=`ses-<完整会话id>`(前 8 位是日期会同日撞名),复用 worktree_manager,会话存储不动;已有消息 400 拒)。(3) 变更面板见上段。REST:`/api/git/{info,checkout,changes,diff}`(`src/web/handlers/git_handler.cpp` 纯函数 + `routes_git.cpp`,cwd 白名单同 files_handler)。测试:`tests/gitinfo/`(纯逻辑 + 真实 git fixture)+ `tests/prompt/git_status_prompt_test.cpp` + 前端 `gitSessionPill/gitChanges.test.js`。

### Desktop shell + multi-workspace

`acecode-desktop.exe`(`-DACECODE_BUILD_DESKTOP=ON` 编出)是个 webview/webview 壳,默认管 N 个 daemon 子进程 — 每 workspace 一个独立 daemon(独立 loopback 端口 + 独立 token + 独立 Job Object)。daemon 内部代码零改动:每个 daemon 进程仍只服务自己的 `current_path()`,通过 `lpCurrentDirectory` 在 `CreateProcess` 时落地。

关键模块(全在 `src/desktop/`):
- `workspace_registry.{hpp,cpp}` — 扫 `.acecode/projects/<hash>/workspace.json`(`{cwd, name}`),默认命名 = `fs::path(cwd).filename()`,行内重命名走 `set_name` 原子写。
- `daemon_pool.{hpp,cpp}` — `unordered_map<hash, Slot>`,per-key condvar 串行化同 hash 并发 activate;`stop_all` best-effort。`IDaemonSupervisor` 虚基类便于单测 mock。
- `pick_active.{hpp,cpp}` — 启动选 active workspace 的纯函数:`state.json::last_active_workspace_hash` → process cwd 的 hash → registry 第一项 → 空。
- `folder_picker_win.cpp` — `IFileOpenDialog` + `FOS_PICKFOLDERS`,COM STA。
- `web_host.{hpp,cpp}` — webview 包装,暴露 `bind/eval/init_script/native_window`,debug=true 默认开 F12 DevTools。
- `tray_icon_win.{hpp,cpp}` / `notifications_win.{hpp,cpp}` / `tray_menu_layout.hpp` — 系统托盘 + OS 气泡通知。tray 注册 hidden **真实顶层** window(WS_POPUP+WS_EX_TOOLWINDOW,无 WS_VISIBLE;**不能用 HWND_MESSAGE** —— message-only 窗口没有前台资格,SetForegroundWindow 静默失败,TrackPopupMenu 菜单会被置顶任务栏挡住且 click-away 不消失;KB135788 配方 = SetForegroundWindow 前置 + 菜单后 PostMessage(WM_NULL)),WndProc 接 `Shell_NotifyIcon` 回调消息(`NIN_BALLOONUSERCLICK` / `WM_LBUTTONUP` / `WM_LBUTTONDBLCLK` / `WM_RBUTTONUP`);通知用 `Shell_NotifyIconW(NIM_MODIFY, NIF_INFO)` piggyback 在同一图标上。`init_tray_icon` → `init_notifications(tray_hwnd)`,顺序不能反。V1 仅气泡,V2 计划接 WinRT ToastNotificationManager(需 AUMID + 开始菜单 .lnk + cppwinrt)。托盘图标走 `LoadImageW(IMAGE_ICON, SM_CXSMICON, SM_CYSMICON)` 直接挑 .ico 内 16×16 frame(`enhance-desktop-tray-menu`)。右键菜单走 Codex 风格(Pinned / Recent + More 子菜单 / 新建会话 / 打开 ACECode / 退出),layout 由 `tray_menu_layout.hpp::compute_menu_layout(payload)` 纯函数算 ID 编码 — 1..49 固定项 / 100..199 pinned / 200..299 recent 顶层 / 300..399 More 子菜单。
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
- `aceDesktop_setWindowBackgroundColor("#rrggbb")` → `{ok}` — 窗口打底色跟随主题(webview resize 黑边治理):native 把 host/`webview_widget` 两层窗口类背景刷 + WebView2 `DefaultBackgroundColor` 一起换,快速拖大窗口时新暴露区域不再闪黑(浅色)/闪白(暗色)。启动静态打底 = `WEBVIEW2_DEFAULT_BACKGROUND_COLOR` 环境变量(必须 FF 前缀 8 位 hex,官方文档:仅属性设置在首帧前仍闪白)+ 注册窗口类时的默认刷(#f5f5f2 = 浅色 `--ace-bg`,纯逻辑在 `src/desktop/window_background.{hpp,cpp}`);前端 `theme.jsx` 的 data-theme effect 经 `lib/desktopWindowBackground.js::pushWindowBackgroundColor` 读 `--ace-bg` computed 值推送(CSS 是颜色单一事实源)。native 端哨兵解析只收 `#RRGGBB`,非法/非 Windows 返回 ok:false 前端静默吞。
- `window.aceDesktop_createNewSession()` — App.jsx 注册的全局函数(非 bind),托盘菜单"新建会话"通过 `host.eval()` 调用,等价于 TopBar 的 onNewSession 路径。

桌面通知抑制规则(`config.desktop.notifications`,默认四个 bool 均 true):`enabled` 总开关 / `on_question` AskUserQuestion 触发 / `on_completion` 回合完成触发 / `suppress_when_focused` 当前 session 已可见且窗口聚焦时跳过(避免对正盯屏的用户重复打扰)。配置经 `/api/health` 透传给前端(`health.notifications`),`lib/desktopNotify.js::maybeNotify` 一站式构造 payload + 抑制 + 投递。**多 workspace v1 限制**:webview 只连 active workspace 的 daemon,后台 workspace 的事件感知不到;v2 future work。

切换 workspace 走**整页 navigate**(对设计稿 D6"无重载切换"的合理偏离):跨 loopback 端口 fetch 受 CORS 拦截,整页 navigate 让 origin 跟着切。代价是滚动 / 弹窗状态丢失,但 input box 清空 / 消息列表替换符合 spec。后续若加 `Access-Control-Allow-Origin: http://127.0.0.1:*` 可恢复无重载。

**webapp 兼容模式**(WebView2 不可用或 `--webapp`):`main.cpp::run_browser_fallback` 用外部浏览器打开 `<url>?ace_webapp=1` —— Edge `--app`(`launch_edge_app`)优先,失败退到系统默认浏览器(`open_external_url`)。**关键**:不再用 `WaitForSingleObject(msedge)` 当生命周期(Chromium single-instance 转交会让被等的进程瞬间退出 → daemon 被 `stop_all` 提前杀 → Edge 窗口连不上 → 白屏)。改为托盘 + Win32 消息循环托住进程,daemon 全程存活;Edge `--app` 这条额外挂 watcher 线程(`WaitForMultipleObjects(msedge, stop_event)`)实现关窗即退出。`launch_edge_app` 每次启动用一个 PID 唯一的干净 `edge-app-profile/u<pid>` 子目录(开机清旧的)杜绝转交 —— 代价是该兜底路径下 Edge profile 的 localStorage(主题/布局偏好)不跨启动持久。前端 `lib/desktopShellMode.js` 在 mount 前把该参数固化到 sessionStorage(`ace.webappCompat`,App 内多处 replaceState 会抹 query,只靠 URL 会丢)并设 `window.__ACECODE_WEBAPP_COMPAT__`;`DesktopContextMenu` 的门控 = `isDesktopShell() || isWebappCompat()`,即兼容模式照常出自定义右键菜单(普通浏览器直连仍保留原生右键)。bridge 依赖项的降级:「在资源管理器中打开/显示」走 `POST /api/open-in-explorer` REST 兜底;剪贴板走 `navigator.clipboard`(loopback 是 secure context);「检查」(DevTools) 仅 debug 且兼容模式不可用。选目录对话框由 daemon 进程弹出(无前台权),`folder_picker_win.cpp` 用 owner=GetForegroundWindow + #32770/pid 匹配的 TOPMOST 脉冲 + AttachThreadInput 组合置前;mac 在 runModal 前升 activation policy 到 Accessory + activate + NSModalPanelWindowLevel;Linux zenity/kdialog 受 WM 焦点防抢限制,无可靠通用解(future: xdg-desktop-portal FileChooser)。

设计文档:`docs/desktop-shell/design.md` + `docs/desktop-shell/multi-workspace.md`,change:`openspec/changes/add-desktop-multi-workspace/`。

### Web UI: 控制台停靠区(ConsoleDock)

`add-console-dock`:TopBar `>_` 按钮(icon `terminal` = TerminalReadWrite.svg)或 ``Ctrl+` `` toggle 主内容列底部的终端停靠区(横跨 ChatView+SidePanel,不动 Sidebar/TopBar)。前端 xterm.js(`@xterm/xterm` + `@xterm/addon-fit`,bundle +~190KB gzip);多 tab(+ 新建 / 逐 tab × 杀会话 / 整体收起保活);顶边拖高(`clampDockHeight`,[120, 视口 80%]);偏好 `acecode.consoleDock.v1` `{open, height}`。纯函数层 `web/src/lib/consoleDock.js`(tab 状态机/帧分流/退避/URL,Node 单测)。**xterm 聚焦时 ``Ctrl+` `` 靠 `attachCustomKeyEventHandler` 放行冒泡** — 删掉它快捷键在终端内失灵。

daemon 侧 `src/web/pty/`:`PtyBackend` 四实现 — **ConPty**(1809+,GetProcAddress 探测;读循环必须 PeekNamedPipe 轮询,`ClosePseudoConsole` 后管道不保证断,阻塞 ReadFile 永不返回)/ **Winpty**(<1809 完整 TTY;winpty 0.4.3 经 FetchContent + `cmake/acecode_winpty.cmake` 自写包装,**不编上游 AgentLocation.cc**,由 `winpty_agent_location.cpp` 提供可覆盖的 `findAgentProgram()`;agent.exe 310KB 嵌入二进制(bin2cpp),首次使用释放到 `<data_dir>/bin/`)/ **Pipe**(兜底,无 TTY 语义,前端显示"兼容模式")/ **PosixPty**(forkpty,Linux `<pty.h>`/macOS `<util.h>`)。`PtySessionRegistry`:2MB 缓冲 + 字节游标续传、`0x00`+JSON 控制帧、16 会话上限、kill 必须锁外(join 读线程回抢 mu_)。路由 `/api/pty` + `/ws/pty/<id>?cursor=N` **loopback 硬限制**(token 不豁免);WS 的 id/cursor 经 Crow `onaccept` 的 `void** userdata` 传递(onopen 拿不到 route 参数)。config `console.shell` 覆盖 shell(默认 `%COMSPEC%`/`$SHELL` — cmd 优先于 powershell 是用户决策,与 bash_tool 一致)。协议文档 docs/daemon-api.md §7。

**ConPTY 必踩坑**:pseudoconsole 子进程的 std 句柄仍按老规则从父进程 PEB 复制 — daemon 被 desktop 以 `CREATE_NO_WINDOW` + NUL std 启动时,cmd 复制到无效 stdin → 读 EOF → 秒退 code 0(实测复现)。修复 = spawn 时 `STARTF_USESTDHANDLES` + 三个 NULL 句柄(node-pty 同款),console 子进程初始化回退绑定 conpty console。该修复同时治好了单测 harness 内的同根伪影(最初误判为受限 Job 杀 conhost);`pty_backend_test.cpp` 的环境探针防御性保留。

### Proxy / 抓包

`src/network/proxy_resolver.{hpp,cpp}` (with platform-specific `_posix` / `_win` bodies) injects proxy options into all cpr call sites uniformly. Design: `openspec/changes/respect-system-proxy`.

`config.network.proxy_mode`:
- `"auto"` (default) — Windows: `WinHttpGetIEProxyConfigForCurrentUser` → HKCU registry → env. POSIX: env (`HTTPS_PROXY`/`HTTP_PROXY`/`ALL_PROXY`/`NO_PROXY`, lowercase variants).
- `"off"` — force direct, ignore system/env.
- `"manual"` — use `proxy_url` (auto-prefixes `http://` if missing).

Both `main.cpp` and `daemon/worker.cpp` call `proxy_resolver().init(cfg.network)` and print `Proxy: <url-redacted-or-direct> (<source>)` (credentials masked). ACECode does not expose TLS verification bypass or custom CA bundle settings; certificate trust is owned by the OS / TLS backend.

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

### LSP(openspec add-lsp-service,仿 opencode)

`src/lsp/` 是自写的 LSP 客户端子系统(零新第三方库;进程段架构同 `codex_app_server_client`,分帧换成 `Content-Length`)。三个能力面:

1. **`lsp` 工具**(9 操作:goToDefinition / findReferences / hover / documentSymbol / workspaceSymbol / goToImplementation / prepareCallHierarchy / incomingCalls / outgoingCalls),`is_read_only=true` 免确认,入参 1-based 行列,结果原样 LSP JSON(0-based)。`config.lsp.enabled=false` 时不注册。
2. **编辑后诊断注入**:file_edit / file_write 成功落盘后 `lsp::append_diagnostics_block`(`src/lsp/lsp_diagnostics.cpp`)—— touch + 等 document 诊断(5s 总超时、150ms push debounce、50ms 粒度轮询 abort),把 **ERROR 级**诊断以 `<diagnostics file=...>` 块(≤20 条)附加到工具输出。未初始化 / disabled / 无匹配 server 零延迟。WARN 以下刻意不透出(避免模型跑偏修风格)。
3. **状态表面**:`/lsp` 双端命令(TUI builtin_commands + daemon `execute_builtin_command` 白名单 + Web 斜杠下拉,三处文本共用 `dispatch_lsp_subcommand`);TUI 侧边栏有连接时渲染 LSP 节(走 `connected_snapshot()`,廉价锁拷贝;完整 `status_snapshot()` 含 which 探测只给 /lsp 用)。

结构:`lsp_frame`(分帧纯状态机)→ `lsp_process`(spawn;**Windows 上 argv[0] 为 .cmd/.bat 时自动经 `cmd.exe /d /c`**,npm shim 必需;探测走 `lsp_which` 的 PATHEXT 逻辑)→ `lsp_client`(reader 线程 + pending map;push/pull 双通道诊断缓存,key 经 `lsp_uri::normalize_path_key` 归一 —— **clangd 会把盘符改小写回推,不归一就永远等不到诊断**)→ `lsp_server_registry`(内置 clangd/typescript-language-server/pyright/gopls/rust-analyzer 定义 + config 合并;root markers 对照 opencode server.ts;ts 需向上找到 `node_modules/typescript/lib/tsserver.js` 否则跳过;rust-analyzer 无 Cargo.toml 判不适用)→ `lsp_service`((server_id,root) 客户端池、per-key slot 锁单飞、broken 集合本进程内不重试)→ 进程级单例 `lsp::init/shutdown/service()`(web_search runtime 同套路;main.cpp 与 worker.cpp 各接一次,退出路径在 mcp shutdown 之后调 `lsp::shutdown()`)。

**workspace 边界两条铁律**(2026-07-08 线上排障后立的,回归测试在 `tests/lsp/lsp_service_test.cpp`):(1) 所有查询/诊断入口按**调用传入的 session_cwd**(`ToolContext::cwd`)判 workspace 边界,空串才回退进程 init cwd —— daemon 单进程经 routes_workspaces 服务多 workspace 会话后,进程 cwd ≠ 会话 cwd,只认进程 cwd 会让其它 workspace 的文件恒报 "No LSP server available"、server 永不 spawn;(2) 边界比较/root 探测/slot key/URI 全部先 `weakly_canonical` 归一 —— 用户目录是 junction 时(实测 `C:\Users\x` → `N:\Users\x`),lsp 工具入口 canonical 过、诊断注入路径没 canonical 过,两形态前缀互不匹配,表现为「编辑后诊断能连上 server,查询却报无 server」。

原则:**只探测已安装的 server(PATH / 项目内),绝不自动下载,server 不进安装包**;探测不到静默跳过零开销。集成测试用 `tests/lsp/helpers/fake_lsp_server.cpp`(BUILD_TESTING 编译,同 mcp_stdio_test_server 思路),不依赖 node / 真实 clangd。

`config.lsp`:`enabled`(默认 true)+ `servers`(按名合并:内置名可 `{"disabled":true}` 或覆盖 command/extensions/env/initialization;新名 = 自定义 server,command 必填,root 恒为 workspace cwd)。

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
    "filenames": ["AGENT.md", "CLAUDE.md"],
    "read_claude_md": true
  },
  "models_dev": {
    "allow_network": false,
    "user_override_path": null,
    "refresh_on_command_only": true
  },
  "input_history": { "enabled": true, "max_entries": 10 },
  "agent_loop": { "max_iterations": 0 },
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
  "lsp": {
    "enabled": true,
    "servers": {
      "clangd": { "disabled": true },
      "ocaml": { "command": ["ocamllsp"], "extensions": [".ml", ".mli"] }
    }
  },
  "worktree": {
    "symlink_directories": ["node_modules"],
    "sparse_paths": []
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

At startup and on every `--resume` / `/resume`, `src/provider/model_resolver.cpp` resolves the effective `ModelEntry` in three layers. CLI `-r` is not a `--resume` short alias; it starts the TUI normally, runs the startup hook path, then opens the same `/resume` picker immediately.
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

`.github/workflows/package.yml` builds Linux x64/arm64, Windows x64/arm64, macOS x64/arm64. Releases auto-cut on `v*` tags.

**npm 发布**(`publish-npm` job,`v*` tag 或带 `npm_version` 的手动 workflow 触发):CLI 主包发布为裸名 `acecode`，桌面主包为 `@aceagent/desktop`，另有 6 个平台二进制包 `@aceagent/<os>-<cpu>`(esbuild 模式:主包只有 JS 垫片,`optionalDependencies` 按 os/cpu 字段精确钉同版本平台包)。平台包同时装 `acecode` / `ace-browser-host` / `acecode-desktop`(macOS 为 `ACECode.app`)——三者都按「自身真实路径所在目录」互相定位(`locate_daemon_exe` / `resolve_host_path`),不能拆包,垫片必须直接 spawn 包内原始文件不能拷走。组装脚本 [scripts/npm/prepare-npm-packages.mjs](scripts/npm/prepare-npm-packages.mjs),主包模板在 [npm/cli/](npm/cli) 与 [npm/desktop/](npm/desktop);改名/换 scope 需同步脚本、模板与两个 bin 垫片。job 行为:`NPM_TOKEN` secret 缺失 → 失败;已发布版本 → 幂等跳过(重跑安全);预发布版本(名含 `-`)→ dist-tag `next`,否则 `latest`。手动 `npm_version` 必须与 `CMakeLists.txt` 项目版本一致，且不会重复创建 GitHub Release。linux-old(glibc ≤ 2.28)产物刻意不上 npm(npm 无法按 glibc 选包),老发行版用户走 GitHub Releases。
