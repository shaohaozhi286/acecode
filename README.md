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

A terminal-based AI coding agent built with C++17. It provides an interactive TUI (Terminal UI) powered by [FTXUI](https://github.com/ArthurSonzogni/FTXUI), supporting tool-use workflows through OpenAI-compatible APIs or GitHub Copilot.

![ACE Code](https://2017studio.oss-cn-beijing.aliyuncs.com/acecode.jpg)
## Features

- **Interactive TUI** -- FTXUI-based terminal interface with streaming responses, input history, and mouse support
- **Agent Loop** -- Multi-turn conversation with automatic tool calling and user confirmation
- **Dual Provider**
  - **OpenAI-compatible** -- Connect to any API that implements the OpenAI chat completions interface (local LLMs, cloud endpoints)
  - **GitHub Copilot** -- Device-flow OAuth authentication with token persistence
- **Built-in Tools**
  - `Bash` -- Execute shell commands
  - `FileRead` / `FileWrite` / `FileEdit` -- Read, create, and patch files
  - `Grep` -- Regex search across files
  - `Glob` -- Find files by glob pattern
- **Cross-platform** -- Windows and Linux/macOS support

## Prerequisites

- CMake >= 3.20
- C++17 compiler (MSVC 2019+, GCC 9+, Clang 10+)
- Git
- [vcpkg](https://github.com/microsoft/vcpkg)

## Build

```bash
# Fetch the FTXUI submodule used by the local overlay port
git submodule update --init --recursive

# Install dependencies for your target triplet
<vcpkg-root>/vcpkg install \
  cpr \
  nlohmann-json \
  ftxui \
  --triplet <triplet> \
  --overlay-ports=$PWD/ports

# Configure (adjust toolchain path and triplet as needed)
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=<triplet> \
  -DVCPKG_OVERLAY_PORTS=$PWD/ports

# Build
cmake --build build --config Release
```

Common triplets:

- `x64-linux`
- `arm64-linux`
- `x64-windows`
- `x64-osx`
- `arm64-osx`

The executable will be output to `build/acecode` on Ninja single-config builds, or under `build/<config>/` on multi-config generators.

## GitHub Actions packaging

The repository includes `.github/workflows/package.yml`, which builds and uploads artifacts for:

- Linux x64
- Linux arm64
- Windows x64
- macOS x64
- macOS arm64

You can trigger it manually with **Actions > package > Run workflow**, or let it run automatically on pull requests, pushes to `main`, and version tags (`v*`).

### Windows note

On Windows, `cpr` depends on libcurl which must be **>= 8.14** for proper TLS certificate handling. The CMake build will fail early if it detects an older version. Make sure your vcpkg checkout is recent enough.

## Configuration

Config file location: `~/.acecode/config.json` (created automatically on first run).

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

| Field | Description |
|-------|-------------|
| `provider` | `"copilot"` or `"openai"` |
| `openai.base_url` | API endpoint URL |
| `openai.api_key` | API key for the endpoint |
| `openai.model` | Model name to request |
| `copilot.model` | Copilot model name (default `gpt-4o`) |

## Usage

```bash
./build/Release/acecode
```



## License

MIT
