```ansi
░█▀█░█▀▀░█▀▀░
░█▀█░█░░░█▀▀░
░▀░▀░▀▀▀░▀▀▀░
```
[![Stars](https://img.shields.io/github/stars/shaohaozhi286/acecode?style=flat-square)](https://github.com/shaohaozhi286/acecode/stargazers)
[![Forks](https://img.shields.io/github/forks/shaohaozhi286/acecode?style=flat-square)](https://github.com/shaohaozhi286/acecode/network/members)
[![Issues](https://img.shields.io/github/issues/shaohaozhi286/acecode?style=flat-square)](https://github.com/shaohaozhi286/acecode/issues)
[![Last Commit](https://img.shields.io/github/last-commit/shaohaozhi286/acecode?style=flat-square)](https://github.com/shaohaozhi286/acecode/commits)

English | [中文](README_CN.md)

**ACECode** is a terminal-based AI coding agent built with C++17. It runs entirely in your shell, talks to either an OpenAI-compatible endpoint or GitHub Copilot, and uses tool-calling to read/edit your project files, run shell commands, and search code on your behalf — all from an interactive TUI powered by [FTXUI](https://github.com/ArthurSonzogni/FTXUI).

![ACE Code](https://2017studio.oss-cn-beijing.aliyuncs.com/acecode.jpg)

## Features

- **Interactive TUI** — streaming responses, input history, mouse support
- **Multi-turn agent loop** — automatic tool calling with per-tool user confirmation
- **Two providers**
  - **OpenAI-compatible** — any endpoint that speaks the OpenAI Chat Completions API (local LLMs, cloud, proxies)
  - **GitHub Copilot** — device-flow OAuth, token persistence, automatic refresh
- **Built-in tools** — `Bash`, `FileRead`, `FileWrite`, `FileEdit`, `Grep`, `Glob`
- **Session persistence** — resume any prior conversation by ID
- **Cross-platform** — Linux, macOS, and Windows binaries

---

## Quick Start

If you just want to try ACECode, grab a prebuilt binary from the [Releases](https://github.com/shaohaozhi286/acecode/releases) page (Linux x64/arm64, Windows x64, macOS x64/arm64) and run:

```bash
# 1. First-time setup — pick a provider and model
./acecode configure

# 2. Launch the agent in your project directory
cd /path/to/your/project
./acecode
```

On first launch you'll be guided through:

- Choosing **GitHub Copilot** (recommended, no API key needed) or an **OpenAI-compatible** endpoint
- For Copilot: a one-time browser-based device login — the token is saved to `~/.acecode/`
- For OpenAI-compatible: enter the `base_url`, `api_key`, and model name

Then type a request in the TUI and press Enter:

```
> refactor src/main.cpp to extract the CLI parsing into its own file
```

The agent will plan, call tools (asking for confirmation on writes/exec), and stream its work back to you.

---

## How to Use

### Command-line flags

```bash
./acecode                     # Start a fresh session in the current directory
./acecode --resume            # Resume the most recent session
./acecode --resume <id>       # Resume a specific session by id
./acecode configure           # Run the interactive setup wizard
./acecode --dangerous         # Skip ALL permission prompts (use with care)
```

> ACECode requires an interactive TTY — it will refuse to start when stdin/stdout are piped.

### Slash commands (in the TUI)

| Command     | Description                                  |
|-------------|----------------------------------------------|
| `/help`     | List all available commands                  |
| `/clear`    | Clear conversation history                   |
| `/model`    | Show or switch the current model             |
| `/config`   | Show the current configuration               |
| `/tokens`   | Show session token usage                     |
| `/compact`  | Compress conversation history to save tokens |
| `/resume`   | Open the session picker to resume a session  |
| `/exit`     | Quit ACECode                                 |

### Built-in tools

The agent decides which tools to call. By default, **read-only tools auto-run** and **write/exec tools prompt** you for approval.

| Tool        | Purpose                              |
|-------------|--------------------------------------|
| `Bash`      | Execute shell commands               |
| `FileRead`  | Read file contents                   |
| `FileWrite` | Create or overwrite files            |
| `FileEdit`  | Apply targeted edits to a file       |
| `Grep`      | Regex search across files            |
| `Glob`      | Find files by glob pattern           |

### Configuration

Config file: `~/.acecode/config.json` (created automatically on first run, or via `acecode configure`).

```json
{
  "provider": "copilot",
  "openai": {
    "base_url": "http://localhost:1234/v1",
    "api_key": "your-api-key",
    "model": "local-model"
  },
  "copilot": {
    "model": "gpt-4o"
  }
}
```

| Field             | Description                                |
|-------------------|--------------------------------------------|
| `provider`        | `"copilot"` or `"openai"`                  |
| `openai.base_url` | API endpoint URL                           |
| `openai.api_key`  | API key for the endpoint                   |
| `openai.model`    | Model name to request                      |
| `copilot.model`   | Copilot model name (default `gpt-4o`)      |

Sessions are stored under `.acecode/projects/<cwd_hash>/` per-project.

### Daemon mode (HTTP / WebSocket server)

ACECode can run as a background daemon exposing the agent loop over HTTP and
WebSocket — useful for the upcoming web UI, CI integrations, or running
ACECode on a headless box you SSH into.

Three ways to start it (pick one):

```bash
# 1. Foreground — log to stderr, blocks the terminal (development / debugging)
./acecode daemon --foreground

# 2. Detached — POSIX double-fork / Windows DETACHED_PROCESS, runs in background
./acecode daemon start
./acecode daemon status        # {pid, port, guid, last_heartbeat_age_ms}
./acecode daemon stop          # graceful SIGTERM + 10 s timeout

# 3. Windows Service — registers with SCM, starts before login screen at boot
#    (Windows only; requires elevated PowerShell for install/uninstall)
./acecode service install
./acecode service start
./acecode service status
./acecode service stop
./acecode service uninstall
```

**Default endpoint**: `http://127.0.0.1:28080` (`web.bind` / `web.port` in
config). Bind failure (port in use) is fail-fast — the daemon does NOT
retry or pick a different port; change `web.port` in `config.json` or
free the port.

**Authentication**: a 32-byte URL-safe base64 token is generated on every
start and written to `<data_dir>/run/token` with restricted permissions
(POSIX `0600` / Windows owner-only DACL). Loopback clients can skip the
token; non-loopback clients must pass `X-ACECode-Token: <token>` header
or `?token=<token>` query.

**Data directory**:
- TUI / `acecode daemon ...` modes use `~/.acecode/`
- `acecode service ...` mode uses `%PROGRAMDATA%\acecode\` (Windows) /
  `/Library/Application Support/acecode/` (macOS) /
  `/var/lib/acecode/` (Linux)
- The two roots are independent — service-mode daemon and TUI do **not**
  share sessions / config / memory in v1

**Quick health check**:
```bash
TOKEN=$(cat ~/.acecode/run/token)        # or %PROGRAMDATA%\acecode\run\token in service mode
curl -sH "X-ACECode-Token: $TOKEN" http://127.0.0.1:28080/api/health | jq
```

Full HTTP / WebSocket protocol reference: [`docs/daemon-api.md`](docs/daemon-api.md).

#### Linux: sample systemd unit

`acecode service install` is Windows-only. On Linux, run the daemon under
systemd. Save as `/etc/systemd/system/acecode.service`:

```ini
[Unit]
Description=ACECode Background Agent
After=network.target

[Service]
Type=simple
ExecStart=/usr/local/bin/acecode daemon --foreground
Restart=on-failure
RestartSec=5
# Run as your normal user; daemon will use ~/.acecode for that user.
User=youruser
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
```

Then: `sudo systemctl daemon-reload && sudo systemctl enable --now acecode`.

#### macOS: sample launchd plist

Save as `~/Library/LaunchAgents/dev.acecode.daemon.plist`:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
  "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>dev.acecode.daemon</string>
  <key>ProgramArguments</key>
  <array>
    <string>/usr/local/bin/acecode</string>
    <string>daemon</string>
    <string>--foreground</string>
  </array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>StandardOutPath</key><string>/tmp/acecode.out.log</string>
  <key>StandardErrorPath</key><string>/tmp/acecode.err.log</string>
</dict>
</plist>
```

Then: `launchctl load ~/Library/LaunchAgents/dev.acecode.daemon.plist`.

### Tips

- Run ACECode from your project root — paths are validated against this directory.
- Use `/compact` when a long session is approaching the context limit.
- `--dangerous` is convenient for sandboxed environments (containers, VMs) but disables all safety prompts.

---

## How to Build

If you want to build from source (e.g., to hack on ACECode or build for an unsupported platform):

### Prerequisites

- CMake **>= 3.20** and Ninja
- A C++17 compiler — MSVC 2019+, GCC 9+, or Clang 10+
- Git
- [vcpkg](https://github.com/microsoft/vcpkg)

### Steps

```bash
# 1. Clone with submodules (the local FTXUI overlay port needs them)
git clone --recursive https://github.com/shaohaozhi286/acecode.git
cd acecode
# (or, if already cloned: git submodule update --init --recursive)

# 2. Install dependencies via vcpkg using the local overlay ports
<vcpkg-root>/vcpkg install \
  cpr nlohmann-json ftxui gtest \
  --triplet <triplet> \
  --overlay-ports=$PWD/ports

# 3. Configure
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=<triplet> \
  -DVCPKG_OVERLAY_PORTS=$PWD/ports

# 4. Build
cmake --build build --config Release
```

Common triplets: `x64-linux`, `arm64-linux`, `x64-windows`, `x64-windows-static`, `x64-osx`, `arm64-osx`.

The binary is written to `build/acecode` (Ninja) or `build/<config>/acecode` (multi-config generators).

### Windows note

On Windows, `cpr` depends on libcurl, which must be **>= 8.14** for proper TLS certificate handling. The CMake build will fail early if it detects an older version — make sure your vcpkg checkout is recent enough.

### CI / packaging

`.github/workflows/package.yml` builds and uploads artifacts for Linux x64/arm64, Windows x64, and macOS x64/arm64. It runs on PRs, pushes to `main`, version tags (`v*`), and via **Actions > package > Run workflow**.

`.github/workflows/test.yml` runs the gtest unit suite on Ubuntu x64 for every PR.

## Testing

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=<triplet> \
  -DBUILD_TESTING=ON
cmake --build build --target acecode_unit_tests
ctest --test-dir build --output-on-failure
```

Tests live under `tests/`, mirror the `src/` layout, and file names end in `_test.cpp`. The `tests/CMakeLists.txt` globs `*_test.cpp` automatically; to add a test for `src/foo/bar.cpp`, drop `tests/foo/bar_test.cpp`. TUI rendering code (`src/tui/*`, `src/markdown/*`, `main.cpp`) is exempted.

---

## License

MIT
