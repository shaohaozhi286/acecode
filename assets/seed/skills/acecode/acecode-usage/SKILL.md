---
name: acecode-usage
description: Explain how to use ACECode itself — CLI flags, config, TUI keybindings, slash commands, daemon/Web UI, models, skills, memory, MCP, proxy, web search. Load this when the user asks how ACECode works, which command does X, or how to configure ACECode.
license: MIT
compatibility: ACECode terminal agent
metadata:
  source_id: acecode:acecode-usage@2026-05-01
  tags: [acecode, usage, reference]
---

# ACECode Usage

ACECode is a C++17 terminal AI coding agent. It runs as an FTXUI TUI by default, and can also run as a long-lived daemon that exposes the same agent loop over HTTP/WebSocket with a built-in browser UI.

Use this skill when the user asks "how do I do X with ACECode", "what does /Y do", "where is Z configured", or "what are the keybindings". When the answer is in this file, give it directly. When the user asks for something this file does not cover, say so and tell them to run `/help` or check `~/.acecode/config.json`.

## CLI

```bash
acecode                          # Fresh TUI session in $PWD
acecode --resume                 # Resume the most recent session for $PWD
acecode --resume <session-id>    # Resume a specific session
acecode configure                # Setup wizard (provider, model, API key)
acecode -dangerous               # Bypass permission checks (Yolo mode)
acecode -alt-screen              # Force alt-screen rendering for legacy terminals
acecode --validate-models-registry   # CI helper, validates models.dev bundle

acecode daemon --foreground      # Run daemon in this terminal
acecode daemon start             # Detach a background daemon
acecode daemon stop|status|logs  # Manage running daemon
acecode service install|start|stop|uninstall   # Windows-only service wrapper
```

`-dangerous` and `--dangerous` are equivalent. `-alt-screen` and `--alt-screen` are equivalent.

## Config file

`~/.acecode/config.json`. Edit by hand or via `acecode configure`. Key fields:

- `provider` — `"openai"` or `"copilot"` (legacy single-profile config; prefer `saved_models`)
- `saved_models` + `default_model_name` — named model registry; entry shape: `{name, provider, base_url, api_key, model}`. Names starting with `(` are reserved.
- `context_window` — auto-derived from model registry; manual override accepted
- `agent_loop.max_iterations` — hard cap on tool-call rounds per turn (default 50)
- `web.enabled` / `web.bind` / `web.port` — daemon HTTP/WS server (default 127.0.0.1:28080)
- `daemon.auto_start_on_double_click` — Windows: double-click launches daemon instead of TUI
- `network.proxy_mode` — `"auto"` (default) / `"off"` / `"manual"`. With `"auto"`, Windows reads system + IE proxy; POSIX reads `HTTPS_PROXY`/`HTTP_PROXY`/`NO_PROXY`.
- `web_search.enabled` / `web_search.backend` — `auto` chooses DuckDuckGo or Bing CN by reachability
- `skills.disabled` / `skills.external_dirs` — skip names; extra scan roots
- `memory.enabled` — cross-session user memory under `~/.acecode/memory/`
- `project_instructions` — controls `ACECODE.md` / `AGENT.md` / `CLAUDE.md` discovery
- `mcp_servers` — keyed by server name; `transport` is `stdio` (default), `sse`, or `http`
- `tui.alt_screen_mode` — `"auto"` / `"always"` / `"never"`
- `input_history.max_entries` — per-cwd input history cap (default 10)

Sessions live under `.acecode/projects/<cwd_hash>/` (one tree per working directory).

## TUI keybindings

- Enter — submit
- Esc — cancel current operation; in shell mode, exit shell mode and clear input
- Ctrl+C — same as Esc when busy; press again on idle to confirm exit
- Ctrl+P — cycle permission mode (Default → AcceptEdits → Yolo)
- Ctrl+E — toggle expanded view on a tool-result row (10-line fold)
- ArrowUp/ArrowDown — focus chat rows (scroll); also history navigation in input
- PgUp/PgDn/Home/End — page through chat or pickers
- Tab — accept slash-command autocomplete
- Shift+drag — universal text-selection fallback when terminal intercepts native selection
- Right-click — copy current selection via OSC 52 (works in tmux when `set -g set-clipboard on`)
- `!` at start of empty input — enter shell mode; the line runs as a one-shot bash command (POSIX) or cmd-equivalent

## Permission modes

- **Default** — read-only tools auto-approve; write/exec prompts unless rule matches
- **AcceptEdits** — file writes/edits auto-approve, bash still prompts
- **Yolo** — everything auto-approves except destructive actions outside the cwd

Mode persists per session. `-dangerous` flag forces Yolo at startup.

## Slash commands

| Command | Purpose |
|---|---|
| `/help` | Show command list and skill count |
| `/clear` | End current session, clear conversation + tokens |
| `/compact` | Compress conversation history into a summary |
| `/model` | Show or switch active model. `/model <name>`, `/model --cwd <name>`, `/model --default <name>` |
| `/config` | Print current effective config |
| `/tokens` | Show session token usage |
| `/resume` | Open the resume picker for prior sessions in this cwd |
| `/rewind` (alias `/checkpoint`) | Rewind to a previous user turn |
| `/mcp` | List/inspect MCP servers and their tools |
| `/skills` | List installed skills; `/skills reload` rescans disk |
| `/memory` | `list` / `view <name>` / `edit <name>` / `forget <name>` / `reload` |
| `/init` | Generate or refresh `ACECODE.md` in $PWD via the LLM |
| `/history` | Show or clear per-cwd input history |
| `/models` | Inspect bundled `models.dev` registry; `/models refresh --network` opt-in |
| `/proxy` | Show effective proxy; `refresh`, `off`, `set <url>`, `reset` |
| `/websearch` | Show backend; `refresh`, `use duckduckgo|bing_cn`, `reset` |
| `/title` | Set or clear the terminal window title |
| `/exit` | Quit ACECode |

User-installed skills also register as `/<skill-name>`.

## Tools (LLM-side)

ACECode exposes these tools to the model:

- `bash_tool` — streamed shell execution with abort, 100KB head+tail truncation, optional `stdin_inputs[]` (POSIX)
- `file_read_tool`, `file_write_tool`, `file_edit_tool`
- `grep_tool`, `glob_tool`
- `AskUserQuestion` — pauses the agent loop for a synchronous prompt; the answer flows back as a tool result
- `web_search` — registered only when `web_search.enabled = true`
- `task_complete` — optional explicit terminator that renders a green "Done: ..." row
- `skills_list`, `skill_view` — discovery and lazy load of `SKILL.md`
- `memory_read`, `memory_write` — cross-session user memory
- `mcp_<server>_<tool>` — every connected MCP server's tools, prefixed by server name

## Daemon and Web UI

```bash
acecode daemon start          # Detach
acecode daemon status         # Heartbeat freshness, port, pid
acecode daemon stop           # Graceful shutdown
```

Daemon binds `127.0.0.1:28080` by default and serves a built-in browser UI at `http://localhost:28080/`. Frontend is vanilla ES modules + Custom Elements + Bootstrap 5; no npm build step. Static assets are embedded into the binary, so the daemon ships self-contained — set `web.static_dir` to a non-empty path to serve files from disk during frontend development.

Auth: loopback requests are unauthenticated; non-loopback requires `X-ACECode-Token` or `?token=`. Combining `-dangerous` with a non-loopback bind is rejected.

Same agent loop, same tools, same SessionRegistry as TUI. Sessions are listed in the sidebar; AskUserQuestion and permission prompts surface as in-page modals over WebSocket.

## Skills

- Read at startup from `~/.acecode/skills`, `~/.agent/skills`, plus any `skills.external_dirs`
- Project-local skills under `<cwd>/.acecode/skills` take precedence over global ones
- Each skill is `<category>/<name>/SKILL.md` with YAML frontmatter (`name`, `description`, optional `metadata.tags`)
- Body is loaded lazily via `skill_view` when the skill is invoked
- First-run seeds `find-skills`, `skill-installer`, `skill-creator`, `native-mcp`, `mcporter`, and `acecode-usage` (this skill) into `~/.acecode/skills`
- After editing a skill, run `/skills reload` instead of restarting

## Memory

`~/.acecode/memory/` holds Markdown entries (`<name>.md`) with `name` / `description` / `type` frontmatter. `type ∈ {user, feedback, project, reference}`. `MEMORY.md` is the index; rebuilt on every `memory_write`. `memory_write` is path-locked to the memory directory even in Yolo mode and is auto-approved.

## Project instructions

ACECode auto-loads `ACECODE.md`, `AGENT.md`, and `CLAUDE.md` from `~/.acecode/` first, then walks `$HOME → cwd` outermost-first, picking at most one file per directory (priority follows `cfg.project_instructions.filenames`). `read_agent_md` and `read_claude_md` toggle the latter two; `ACECODE.md` has no toggle. Files are concatenated under `# Project Instructions` after the tool description block in the system prompt.

## MCP

Configured via `mcp_servers` in `config.json`. Three transports:

- `stdio` (default if `transport` is omitted) — `command` + `args` + optional `env`
- `sse` — legacy two-endpoint Server-Sent Events
- `http` — 2025-03-26 Streamable HTTP single endpoint (default `/mcp`)

After editing MCP config, restart ACECode (TUI) or `PUT /api/mcp` followed by `daemon` reload. Each tool registers as `mcp_<sanitized_server_name>_<tool_name>`.

## Troubleshooting

- **Models picker shows `(legacy)`** — `saved_models` is empty or `default_model_name` doesn't match; falls back to top-level `provider` / `openai.*` / `copilot.*` block.
- **`(session:<id>)` ad-hoc entry on resume** — resumed session's `provider`+`model` tuple isn't in `saved_models`; the entry borrows from `cfg.openai`. Add a matching entry to silence the warning.
- **Banner stacks / viewport drifts on Windows** — legacy conhost or ConEmu/Cmder; pass `-alt-screen` once or set `tui.alt_screen_mode = "always"`.
- **Proxy shows `[INSECURE]`** — `network.proxy_insecure_skip_verify = true`. Prefer `network.proxy_ca_bundle` (PEM path) when trusting Fiddler/Charles roots.
- **Web search returns no results** — backend may be regionally blocked. `/websearch refresh` reprobes; `/websearch use bing_cn` forces the China-side backend.
- **Daemon won't start, "address in use"** — port 28080 is busy. Change `web.port` or stop the other process; ACECode does not fall back to a different port.
- **First-run seed skills missing on upgrade** — seed install only runs when `~/.acecode/` is created by the current process. Already-initialized homes don't auto-pick-up new seed skills; copy them manually from `<install-prefix>/share/acecode/seed/skills/` if needed.

## Where to look in the repo

- TUI entry, event loop, layout — `main.cpp`
- Agent loop, tool dispatch — `src/agent_loop.{hpp,cpp}`, `src/tool/`
- Providers — `src/provider/` (`openai_provider`, `copilot_provider`, `model_resolver`)
- Sessions, replay, rewind — `src/session/`
- Daemon, HTTP/WS server, handlers — `src/daemon/`, `src/web/`
- Skills, memory, slash commands — `src/skills/`, `src/memory/`, `src/commands/`
- Config schema and migration — `src/config/`

For deeper architectural context, see `CLAUDE.md` at the repo root.
