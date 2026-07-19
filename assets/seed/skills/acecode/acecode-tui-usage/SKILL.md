---
name: acecode-tui-usage
description: "Explain how to use ACECode in a terminal, including the interactive TUI, headless prompt mode, sessions, worktrees, permission modes, keyboard controls, slash commands, skills, MCP, memory, and troubleshooting. Use when a user asks how to start, control, configure, or recover ACECode from a shell. Do not use for the native ACECode Desktop interface."
---

# ACECode TUI Usage

Give instructions for ACECode's terminal surfaces. Keep TUI guidance separate from
ACECode Desktop guidance, because their commands, controls, and supported features
are not identical.

## Answer From the Current Surface

- Treat `acecode` with no prompt option as the interactive terminal TUI.
- Treat `acecode -p ...` as non-interactive headless prompt mode.
- Treat `acecode daemon ...` as daemon administration, not as the interactive TUI.
- Lead with the shortest working command or key sequence, then explain relevant
  alternatives.
- Preserve the user's shell conventions. Use PowerShell examples on Windows.
- Warn before suggesting `--dangerous`; it bypasses more safety checks than the
  in-session permission modes.
- If the repository is available and a mutable detail matters, verify it in:
  - `src/main.cpp` and `src/cli/interactive_options.cpp` for interactive startup
  - `src/headless/headless_options.cpp` for `-p`
  - `src/commands/builtin_commands.cpp` for slash commands
  - `src/permissions.hpp` for permission behavior

## Start the Interactive TUI

Use:

```text
acecode
```

Useful startup forms:

```text
acecode --resume
acecode --resume <session-id>
acecode -r
acecode --worktree
acecode --worktree <name>
acecode --worktree #123
acecode --alt-screen
```

- `--resume` resumes a session; without an ID it uses the most appropriate recent
  session.
- `-r` opens the session picker.
- `-w` and `--worktree` opt into an isolated Git worktree. ACECode does not create
  a worktree automatically.
- A worktree name may also be a GitHub pull request reference such as `#123` or a
  GitHub pull request URL.
- Worktrees live under `.acecode/worktrees/` and use a generated
  `worktree-<name>` branch.
- On exit, ACECode removes an unchanged temporary worktree. It keeps a worktree
  that has changes, or whose status cannot be determined, and reports its path.
- `--alt-screen` uses the terminal alternate screen buffer.

Use `acecode configure` for guided provider and model setup. Use
`acecode version` to print the version and `acecode upgrade` to check for and apply
an upgrade.

## Send a Headless Prompt

Use headless mode for scripts, pipelines, and one-shot requests:

```text
acecode -p "Explain this repository"
acecode -p --output-format json "List the failing tests"
Get-Content .\request.txt | acecode -p
```

Common options:

- `-c` or `--continue`: continue the most recent compatible session.
- `--resume <id>`: resume a specific session.
- `--session-id <id>`: use an explicit session ID.
- `--output-format text|json|stream-json`: choose result framing.
- `--thinking`: include thinking events where supported.
- `--model <model>`: override the configured model.
- `--permission-mode default|accept-edits|plan|yolo`: select a permission mode.
- `--max-turns <number>`: cap agent turns.
- `--disable-tools`: run without built-in tools.
- `--enable-skills`: enable skill discovery in headless mode.
- `--enable-mcp`: enable configured MCP servers in headless mode.

Headless mode enables normal system tools by default, but skills and MCP are
disabled unless explicitly enabled.

## Use Permission Modes

Press `Shift+Tab` in the TUI to cycle through permission modes:

1. `default`: ask before writes and shell execution.
2. `accept-edits`: automatically allow file edits, but still ask before shell
   execution.
3. `yolo`: automatically approve normal tool permissions; an outside-workspace
   write can still receive a separate safety confirmation.
4. `plan`: investigate and maintain the active plan without implementing code.

Use `/mode` to inspect or change the mode explicitly.

Do not describe `Ctrl+P` as the permission shortcut. It navigates upward in an
open slash-command menu. Do not equate `yolo` with startup `--dangerous`:
`--dangerous` bypasses normal permission handling and path safety and should be
used only in a deliberately isolated environment.

## Control the Composer

- `Enter`: submit the current prompt.
- `Up` and `Down`: browse input history when the current editor state allows it.
- `Ctrl+A` or `Home`: move to the start of the input.
- `Ctrl+E` or `End`: move to the end of the input when no tool result is focused.
- `Ctrl+V`: paste clipboard text.
- `Alt+V`: attach an image from the clipboard.
- Start an empty prompt with `!` to enter direct shell-command mode. Shell commands
  still follow the current permission policy.
- Type `@` to reference a file or folder. Use `Up` and `Down` to select,
  `Enter` to add the reference, and `Right` or `Tab` to descend into a folder.

When the model is already working, submitting another message queues it. Use
`/turn`, not a queued ordinary message, when the intent is to alter the active
turn at its next model boundary.

## Cancel or Exit

- `Escape`: close a picker, reject a pending permission prompt, cancel the active
  turn, leave shell mode, or clear attachments when appropriate.
- `Ctrl+C` while busy: cancel the active turn.
- `Ctrl+C` while idle: first clear input and arm exit; press it again within the
  displayed interval to exit.
- `/exit`: exit explicitly.

Do not promise that cancellation can undo tool actions that already completed.

## Navigate Output and Attachments

- `PageUp` and `PageDown`: scroll output. The default is precise single-line
  stepping.
- `/page-step off`: switch PageUp/PageDown to page-sized movement.
- `Alt+Up` and `Alt+Down`: use single-line scrolling where supported.
- `Ctrl+O`: expand or collapse all tool output.
- `Ctrl+E` with a focused tool result: expand or collapse that result.
- `Alt+A`: move focus between the transcript and attachments.
- In attachment focus, use `Up` and `Down` to select, `Delete` or `Backspace` to
  remove, and `Escape` or `Alt+A` to return.
- A right-click copies selected transcript text; without a selection it pastes.

In a slash-command menu, use `Up`/`Down` or `Ctrl+P`/`Ctrl+N` to navigate and
`Enter` to select.

## Use Slash Commands

Use `/help` to inspect commands in the installed version. Important command groups
include:

### Session and Context

- `/clear` or `/new`: start with a clear conversation.
- `/resume`: choose or resume a saved session.
- `/rewind` or `/checkpoint`: return to an earlier checkpoint.
- `/compact`: compact conversation context.
- `/history`: inspect input or session history.
- `/title`: change the current session title.
- `/tokens`: inspect token usage.

### Model, Mode, and Configuration

- `/model`: choose a model.
- `/models`: refresh or inspect available models.
- `/mode`: inspect or change the permission mode.
- `/config`: inspect or edit configuration.
- `/proxy`: configure or inspect proxy behavior.
- `/theme`: select a TUI theme.
- `/page-step`: control PageUp/PageDown stepping.

### Active Work and Side Questions

- `/goal`: create, inspect, pause, resume, edit, or clear a long-running goal.
- `/plan`: enter Plan mode.
- `/turn <guidance>`: steer the active main turn at its next model boundary.
- `/btw <question>` or `/side <question>`: ask a detached one-turn side question.
  These do not steer or replace the active main turn.
- `/tasks`: list, abort, or clear background subagent tasks.

### Integrations and Project Setup

- `/init`: create or refresh project-level agent instructions.
- `/skills`: list, inspect, or reload discovered skills.
- `/mcp`: inspect and manage configured MCP servers.
- `/memory`: inspect or manage persistent memory in the TUI.
- `/lsp`: inspect language-server integration.
- `/browser`: start or inspect browser automation support.
- `/websearch`: inspect web-search behavior.
- `/remote-control` or `/rc`: manage remote-control access.
- `/feedback`: submit product feedback.

Do not assume a slash command is supported merely because it exists in Desktop or
an older build. Use `/help` or inspect the current command registry when exact
compatibility matters.

## Use Skills

ACECode discovers skills from project and user locations, including:

- `.acecode/skills/`
- `.agent/skills/`
- `~/.acecode/skills/`
- `~/.agent/skills/`
- compatible `.agents/skills/` and OpenCode locations when compatibility is
  enabled
- directories configured in `skills.external_dirs`

Put each skill in its own directory with a `SKILL.md` file. Use `/skills reload`
after adding or editing a skill while ACECode is running. Prefer a precise skill
description so ACECode can select it for the right requests.

## Use MCP and Browser Support

- Configure MCP servers in ACECode configuration, then use `/mcp` to inspect their
  current state and available runtime actions.
- Use `/browser` to start the browser bridge. Once started, the model interacts
  with it through the ACE browser host tooling.
- If an integration was changed outside the running process, reload it when the
  command supports reload; otherwise restart ACECode.

## Find Sessions and Configuration

ACECode stores user configuration, session data, logs, installed seed skills, and
related runtime state under `~/.acecode/`. Sessions are scoped to a project
identity, so changing the workspace path can change which recent sessions appear.

When resume behavior is surprising:

1. Confirm the current directory.
2. Use `acecode -r` or `/resume` instead of relying on automatic selection.
3. Check whether the original session used a worktree or a different project path.
4. Inspect logs under `~/.acecode/` before deleting any state.

## Troubleshoot the TUI

- If text width or colors look wrong, confirm the terminal supports UTF-8 and
  modern ANSI control sequences, then try another theme.
- If a key appears ineffective, close any open picker first; key behavior is
  context-sensitive.
- If an operation is waiting, look for an active permission or question prompt.
- If tools are unavailable in headless mode, check `--disable-tools`,
  `--enable-skills`, and `--enable-mcp`.
- If a worktree remains after exit, inspect it for changes before removing it.
- If behavior differs from this skill, trust `acecode --help`, `/help`, and the
  installed build; then update this skill from the current source.
