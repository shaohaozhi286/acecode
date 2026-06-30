---
name: acecode-usage
description: "Explain how to use ACECode itself: CLI flags, config, TUI keybindings, slash commands, daemon/Web/Desktop, models, skills, memory, MCP, hooks, remote control, browser bridge, attachments, proxy, web search, upgrade, feedback, and troubleshooting. Use when the user asks how ACECode works, which command does something, or how to configure/use an ACECode feature."
license: MIT
metadata:
  source_id: acecode:acecode-usage@2026-06-28
  tags: [acecode, usage, reference]
---

# ACECode Usage

ACECode is a C++17 AI coding agent with several surfaces around one shared agent core: terminal TUI, daemon HTTP/WebSocket API, React Web UI, Windows service mode, and an optional desktop shell.

Use this skill to answer "how do I do X in ACECode?", "what does /Y do?", "where is Z configured?", or "why does this ACECode mode behave this way?". Answer directly from this file when possible. If a detail may have changed, check the current repo docs first: `README.md`, `CLAUDE.md`, `docs/daemon-api.md`, `docs/hooks.md`, `docs/skills.md`, `docs/ace-browser-bridge.md`, and `docs/desktop-shell/multi-workspace.md`.

## CLI

```bash
acecode                              # Fresh TUI session in the current directory
acecode -r                           # Start normally, then open the /resume picker
acecode --resume                     # Resume the latest session for this cwd
acecode --resume <session-id>        # Resume a specific session
acecode configure                    # Setup wizard for provider/model/upgrade URL
acecode version|--version|-version   # Print acecode version
acecode upgrade [--force] [--server <url>]  # Self-upgrade from aceupdate.json
acecode --alt-screen                 # Force alternate-screen rendering this launch
acecode --yolo                       # Dangerous startup mode; same as --dangerous

acecode daemon --foreground          # Run daemon in the current terminal
acecode daemon start                 # Spawn detached daemon
acecode daemon stop|status           # Manage detached daemon

acecode service install|start|status|stop|uninstall  # Windows SCM service
```

`-r` is not a `--resume` short alias. It performs normal startup first, including startup hooks, then opens the same resume picker as `/resume`.

`--yolo`, `-yolo`, `--dangerous`, and `-dangerous` are equivalent startup flags. They set dangerous mode, which bypasses permission and path-safety checks. This is stronger than switching an existing session to `/mode yolo`. Daemon startup rejects dangerous mode when bound to non-loopback.

## Data And Config

Primary user data lives under `~/.acecode/`:

- `config.json` - provider, saved models, permissions, skills, memory, web, daemon, hooks, browser bridge, remote control, upgrade, and desktop preferences
- `projects/<cwd_hash>/` - session JSONL, metadata sidecars, input history, attachments, usage ledgers, and workspace metadata
- `memory/` - Markdown memory entries plus `MEMORY.md` index
- `run/` - daemon pid, port, guid, token, and heartbeat files
- `logs/` - daemon/desktop logs
- `hooks.json` and `hooks_state.json` - legacy hooks and trust/disable state

Windows service mode uses a platform service data directory instead of the normal user `~/.acecode/`, so its sessions/config/memory are separate from TUI user mode.

Important `config.json` areas:

- `saved_models` and `default_model_name` - named model profiles. OpenAI-compatible entries can set `base_url`, `api_key`, `model`, `stream_timeout_ms`, and `request_headers`; `{env:NAME}` placeholders are resolved at request time.
- `context_window` and `models_dev` - context-window resolution. Known providers prefer bundled `models.dev` metadata; generic OpenAI-compatible endpoints may warm `/models` metadata in the background.
- `default_permission_mode` - default for new daemon/Web/Desktop sessions: `default`, `accept-edits`, `plan`, or `yolo`.
- `custom_instructions` and `project_instructions` - request-local prompt context. Project instructions load `AGENT.md` first and `CLAUDE.md` as a compatibility fallback according to configured filename priority and byte caps.
- `skills.disabled` and `skills.external_dirs` - hide skills or add extra scan roots.
- `features.hooks` - enable/disable Codex-compatible hooks; legacy startup hooks keep their compatibility behavior.
- `mcp_servers` - MCP server definitions using `stdio`, `sse`, or Streamable HTTP `http`.
- `web.bind`, `web.port`, `web.static_dir` - daemon/Web UI serving.
- `remote_control` - TUI channel hand-over listener, token, outbound webhook, default channel plugin, and channel manifests.
- `ace_browser_bridge` - browser automation host settings. Current built-in tool exposure is progressive: only `browser_start` is registered.
- `desktop.close_to_tray` and `desktop.notifications` - desktop shell close behavior and OS notification rules.
- `upgrade.base_url` and `upgrade.timeout_ms` - self-upgrade and `/feedback` upload service settings.

`save_config` writes sparse JSON: default-valued fields may be omitted.

## TUI Basics

- Enter - submit current input
- Esc - cancel/deny active overlay; in shell mode, leave shell mode and clear input
- Ctrl+C - cancel when busy; press again while idle to exit
- Ctrl+P - cycle permission modes: Default -> AcceptEdits -> Yolo -> Plan -> Default
- Ctrl+E - expand/collapse focused tool result row
- ArrowUp/ArrowDown - navigate chat rows or input history depending on focus
- PgUp/PgDn/Home/End - page through chat or picker lists
- Tab - accept slash-command autocomplete
- Ctrl+V/right-click with no selection - paste text from system clipboard when available
- Clipboard image paste - stores an image attachment when the platform helper is available
- Alt+A - focus pending attachments; Up/Down selects one; Delete/Backspace removes it
- Shift+drag - terminal text-selection fallback when mouse tracking intercepts drag
- Right-click with a non-empty selection - copy via OSC 52
- `!` at the start of empty input - enter one-shot shell mode

Use `/page-step` when a terminal swallows Alt+Arrow or page scrolling is too coarse. Use `/theme dark|light|auto` to switch the TUI palette; `auto` takes effect on next launch after terminal-background detection.

## Permission Modes

- `default` - read-only tools auto-approve; writes and shell execution prompt.
- `accept-edits` - file writes/edits auto-approve; shell execution still prompts.
- `yolo` - session mode that auto-allows normal tools but confirms the first external file write.
- `plan` - lets the agent explore and update only the active plan file, then call `ExitPlanMode` for user approval before coding.

Use `/mode` to show the current and default modes. Use `/mode <mode>` for the current session, `/mode default <mode>` or `/mode --default <mode>` for new sessions.

Startup `--yolo` / `--dangerous` is not the same as session `/mode yolo`: it bypasses all permission and path-safety checks.

## Slash Commands

| Command | Purpose |
| --- | --- |
| `/help` | Show command list and installed skill count |
| `/clear` / `/new` | End current session and start a clean one |
| `/model` | Show/switch model; supports `/model <name>`, `/model --cwd <name>`, `/model --default <name>` and model-management subcommands |
| `/models` | Inspect bundled/resolved models.dev metadata; lookup model ids |
| `/mode` | Show or change permission mode |
| `/config` | Show effective provider/model/context/permission configuration |
| `/tokens` | Show token usage for the session |
| `/goal` | Create, view, pause, resume, edit, or clear a long-running thread goal |
| `/plan` | Enter plan mode, optionally with a task prompt |
| `/compact` | Compact model-facing history while preserving a human-visible transcript marker/checkpoint |
| `/resume` | Open the resume picker for sessions in this cwd |
| `/rewind` / `/checkpoint` | Rewind to a previous user turn and restore tracked file checkpoints |
| `/mcp` | List/enable/disable/reconnect MCP servers and list their tools |
| `/skills` | List installed skills; `/skills reload` rescans disk |
| `/memory` | Manage persistent memory: list/view/edit/forget/reload |
| `/init` | Generate or improve `AGENT.md` in the current directory |
| `/history` | List or clear per-cwd input history |
| `/proxy` | Show, refresh, disable, set, or reset HTTP proxy settings |
| `/websearch` | Show, refresh, or switch DuckDuckGo/Bing-CN search backend |
| `/remote-control` / `/rc` | Hand a TUI session to a channel plugin or manual webhook bridge |
| `/feedback` | Package current session diagnostics and upload to `upgrade.base_url` |
| `/browser` | Show/toggle ACE Browser Bridge for the current TUI session |
| `/title` | Set/show terminal title |
| `/page-step` | Toggle PgUp/PgDn single-line vs page scrolling |
| `/theme` | Switch TUI theme: `dark`, `light`, `auto` |
| `/exit` | Quit ACECode |

Installed skills also register slash commands as `/<skill-name>`.

## LLM Tools

Common built-in tools:

- `bash` - streamed shell execution with abort support and head/tail truncation.
- `file_read`, `file_write`, `file_edit` - safe file operations. `file_write` and `file_edit` require a prior read for existing files and call the rewind checkpoint hook before mutation.
- `grep`, `glob` - repository search helpers.
- `task_complete` - optional explicit completion signal.
- `TodoWrite` - publish/update the session todo list.
- `get_goal`, `create_goal`, `update_goal` - manage thread-goal state.
- `EnterPlanMode`, `ExitPlanMode` - model-side plan-mode workflow.
- `AskUserQuestion` - structured follow-up prompt; not a task terminator.
- `vision_analyze` - one-shot internal call to a saved model tagged with vision capability; accepts `image_path`, `attachment_id`, or attachment metadata.
- `skills_list`, `skill_view` - discover skills and lazily load `SKILL.md` or supporting files.
- `memory_read`, `memory_write` - TUI memory tools. Current daemon/Web/Desktop worker wiring still passes no `MemoryRegistry`, so do not assume memory works identically there unless the code has been updated.
- `web_search` - registered only when `web_search.enabled` is true.
- `browser_start` - registered when ACE Browser Bridge is enabled. It starts/checks `ace-browser-host` and injects CLI instructions; subsequent browser actions use `ace-browser-host(.exe)` commands, not a batch of model-visible `browser_*` tools.
- `mcp_<server>_<tool>` - tools exposed by configured MCP servers.

Tool results can include summaries, diff hunks, and structured output attachments so TUI/Web resume can render compact useful rows.

## Daemon, Web UI, And Desktop

Daemon:

```bash
acecode daemon start
acecode daemon status
acecode daemon stop
```

Default bind is `127.0.0.1:28080`. Loopback HTTP requests bypass daemon auth. Non-loopback clients must pass `X-ACECode-Token` or `?token=` from `<data_dir>/run/token`, and dangerous mode with non-loopback bind is rejected.

Web UI:

- Served by daemon at `http://127.0.0.1:28080/`.
- Frontend is React 18 + Vite + Tailwind v4, embedded from `web/dist` by CMake.
- Build `web/` with `pnpm install` then `pnpm build` before configuring CMake when a full embedded UI is needed.
- If `web/dist` is missing at configure time, the binary embeds a minimal fallback page; APIs still work.
- `web.static_dir` serves frontend assets from disk for development.
- Web/Desktop prompt flows use WebSocket events for tool progress, permission prompts, AskUserQuestion, todos, goals, and transcript updates.
- `POST /api/sessions/:id/attachments` stores session-scoped attachments; message submit can reference attachment ids and structured browser contexts.

Desktop shell:

- Built with `-DACECODE_BUILD_DESKTOP=ON` as `acecode-desktop` or `ACECode.app`.
- Wraps the Web UI in a native webview, manages workspaces, tray, notifications, folder picker, and bridge calls.
- Current multi-workspace model uses one shared daemon slot and workspace-scoped session APIs; each session still carries its own cwd/tool context.
- Workspace metadata lives under `~/.acecode/projects/<cwd_hash>/workspace.json`.
- Close-to-tray defaults on; set `desktop.close_to_tray=false` to exit on window close.
- Notifications can fire for AskUserQuestion and completion, with `suppress_when_focused` to avoid notifying the visible focused session.
- If WebView2 is unavailable, Windows can fall back to Edge app/webapp compatibility mode; bridge-dependent actions use REST/navigator fallbacks where available.

## Skills

Scan order is project-local first, then global, then configured external dirs:

1. From cwd upward to but not including home: `<dir>/.acecode/skills/`
2. From cwd upward to but not including home: `<dir>/.agent/skills/`
3. User global: `~/.acecode/skills/` (created automatically)
4. Compatible global: `~/.agent/skills/`
5. `config.skills.external_dirs`

Each skill is a directory containing `SKILL.md` with at least `name` and `description` frontmatter. Full bodies load lazily via `skill_view` or explicit `/<skill-name>` invocation.

ACECode also injects a compact proactive skill index into the per-request session context so the model can discover likely skills before calling `skills_list`. The index is budgeted and degrades to names-only when large.

First-run seed bundle currently contains:

- `find-skills`
- `skill-installer`
- `skill-creator`
- `native-mcp`
- `mcporter`
- `acecode-usage`
- `vision-image-reader`

Seed install only runs when the current ACECode process creates `~/.acecode/`. Existing homes do not auto-pick-up new seed skills on upgrade. Copy from `<install-prefix>/share/acecode/seed/skills/` or this repo's `assets/seed/skills/` if needed.

After editing a skill, run `/skills reload`.

## Memory

Memory entries live under `~/.acecode/memory/` as Markdown with frontmatter:

```yaml
---
name: example
description: Short discovery text
type: user
---
```

Allowed `type` values are `user`, `feedback`, `project`, and `reference`. `MEMORY.md` is the index and is rewritten on upsert/remove. `memory_write` is locked to the memory directory even under broad permission modes.

Current caveat: memory is wired into the TUI path. If the user asks whether Web/Desktop/daemon memory recall works, inspect `src/daemon/worker.cpp` and the current tests before claiming parity.

## Project Instructions And Custom Instructions

ACECode builds request-local prompt context from:

- configured custom instructions (`custom_instructions.text`, also editable through Web/Desktop Settings)
- global and project instruction files (`AGENT.md`, optionally `CLAUDE.md` as a fallback)
- the proactive skill index
- the memory index when memory is enabled and wired
- hook-provided request context
- current todos/goals when present

Instruction discovery is bounded by configured max depth and byte caps. Do not create duplicate root instruction files in this repository; keep canonical root docs small.

## MCP

Configure MCP servers in `mcp_servers`:

- `stdio` - `command`, `args`, optional `env`; default when `transport` is omitted
- `sse` - legacy two-endpoint SSE transport
- `http` - Streamable HTTP single endpoint, default `/mcp`

Tools register as `mcp_<sanitized_server_name>_<tool_name>`. Use `/mcp` for status, `/mcp list` for tools, `/mcp enable|disable|reconnect <name>` for runtime management. In daemon/Web, config edits through `/api/mcp` do not hot-reload running clients; restart or reconnect according to the current route support.

## Hooks

ACECode supports Codex-compatible local lifecycle hooks plus legacy ACECode hooks.

Config locations:

1. `~/.acecode/hooks.json` (legacy ACECode)
2. `~/.codex/hooks.json`
3. `<workspace>/.acecode/hooks.json`
4. `<workspace>/.codex/hooks.json`

Codex-compatible hooks are enabled by default and can be disabled with `features.hooks=false`. Non-managed command hooks require trust review before running. Desktop/Web Settings exposes hook refresh/trust/disable/enable; daemon routes are `/api/hooks`, `/api/hooks/refresh`, and `/api/hooks/<id>/trust|disable|enable`.

Supported Codex-style events include `SessionStart`, `UserPromptSubmit`, `PreToolUse`, `PostToolUse`, `Notification`, `PreCompact`, `PostCompact`, and `Stop`. Legacy events include `startup.before_model_load`, `startup.models_loaded`, and `assistant.message_completed`.

## Remote Control

`/remote-control` or `/rc` hands the current TUI session to an external local channel bridge. Inbound messages POST to `http://127.0.0.1:<port>/rc/send` with `X-ACECode-RC-Token` or `?token=`, become normal user turns, persist to JSONL, and queue if a turn is already running. Assistant replies are posted to the configured outbound webhook after each turn.

Use:

- `/rc` - activate configured `remote_control.default_channel`
- `/remote-control on` - manual webhook pairing
- `/remote-control url <webhook-url>` - set outbound webhook
- `/remote-control show` - status, pairing, and counters
- `/remote-control off` - stop listener and detach plugin

The listener is loopback-only and requires its token even on loopback.

## ACE Browser Bridge

Enable with `ace_browser_bridge.enabled=true` or TUI `/browser on` for the current session. Web Settings writes the global config and syncs daemon tools.

Important current behavior:

- ACECode registers only `browser_start` as a model-visible tool.
- `browser_start` checks/starts `ace-browser-host(.exe)` and injects a prompt telling the model to use the host CLI.
- Preferred backend is host-managed direct CDP; extension backend remains a fallback.
- The host manages a persistent Chrome profile under `~/.acecode/browser/chrome-profile` unless overridden by `ACE_BROWSER_USER_DATA_DIR`.
- Use `ace-browser-host read-page` first, then interact with `@e` element refs or structured locators.
- Screenshots and PDFs are saved as files; use `vision_analyze` only when image inspection is required and a vision-capable saved model exists.

See `docs/ace-browser-bridge.md` for CLI commands and diagnostics.

## Attachments And Vision

ACECode supports structured input/output attachments:

- TUI clipboard image paste and Web attachment upload store blobs under the session attachment directory.
- Image MIME types are normalized and may be compressed; SVG is treated as a file, not a raster image for vision routing.
- User input can contain text plus file/image content parts.
- `vision_analyze` can analyze the latest image attachment, a specific `attachment_id`, explicit attachment metadata, or a local `image_path`.
- Tool results may return output image attachments; Web/TUI resume renders fallback links/metadata when direct rendering is unavailable.

## Search, Proxy, And Network

- `network.proxy_mode` is `auto`, `off`, or `manual`; `/proxy` shows and changes the effective proxy.
- Web search is optional. `web_search.backend=auto` selects DuckDuckGo or Bing-CN by cached region/probe; `/websearch refresh` reprobes, `/websearch use duckduckgo|bing_cn` overrides for the current config.
- OpenAI-compatible saved models can set `request_headers`. Do not store real secrets in config when `{env:NAME}` placeholders work.
- ACECode does not provide a TLS verification bypass; install capture proxy root CAs into the OS trust store visible to the process.

## Upgrade And Feedback

`acecode upgrade` reads `aceupdate.json` from `upgrade.base_url`, selects the best package for the current target, verifies size/checksum, stages it, and applies it through the platform updater path. `--force` allows reinstalling the selected package. `--server <url>` overrides the configured upgrade server for this invocation.

`/feedback` packages current session diagnostics and uploads them to the same configured service base. Package filenames include session id, timestamp, platform, and when available machine/login suffixes for traceability.

## Troubleshooting

- Empty model picker: configure `saved_models`; current selection no longer relies on top-level legacy provider fields except for migration/fallback.
- Resumed `(session:<id>)` model entry: session provider/model metadata is not present in `saved_models`; add a matching saved model and set `/model --default`.
- Daemon address in use: port 28080 is busy. Change `web.port` or stop the other process; daemon does not auto-pick a fallback port.
- Non-loopback daemon plus dangerous mode: rejected at startup by design.
- Web UI is a fallback page: build `web/` and reconfigure so `web/dist` is embedded, or set `web.static_dir` while developing.
- Browser bridge exposes only `browser_start`: this is intentional progressive disclosure; use `ace-browser-host(.exe)` CLI after the start prompt.
- AskUserQuestion still appears in Yolo/dangerous startup: yolo skips permissions, not interactive question tools.
- Memory seems absent in Web/Desktop: verify daemon memory wiring; current worker path may not attach `MemoryRegistry`.
- Seed skills missing after upgrade: first-run seeding does not run for an existing `~/.acecode/`.
- Windows desktop relink fails with `LNK1168`: a running `acecode-desktop.exe` likely still holds the output binary.
- Web search returns no results: backend may be regionally blocked; run `/websearch refresh` or force `bing_cn`.

## Repo Pointers

- TUI startup and event loop: `src/tui/`, historical entry logic in the main executable path
- Agent loop and tool dispatch: `src/agent_loop.*`, `src/tool/`
- Built-in slash commands: `src/commands/`
- Config schema and persistence: `src/config/`
- Providers and model resolution: `src/provider/`, `docs/model-context-resolution.md`
- Sessions, replay, compact checkpoints, rewind: `src/session/`
- Skills: `src/skills/`, `docs/skills.md`, `docs/skills-implementation.md`
- Memory: `src/memory/`
- Daemon and Web API: `src/daemon/`, `src/web/`, `docs/daemon-api.md`
- React Web UI: `web/src/`
- Desktop shell: `src/desktop/`, `docs/desktop-shell/multi-workspace.md`
- Hooks: `src/hooks/`, `docs/hooks.md`
- Browser bridge: `src/tool/ace_browser_bridge/`, `docs/ace-browser-bridge.md`
- Upgrade and feedback: `src/upgrade/`, `src/feedback/`
