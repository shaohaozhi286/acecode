```ansi
░█▀█░█▀▀░█▀▀░
░█▀█░█░░░█▀▀░
░▀░▀░▀▀▀░▀▀▀░
```

# ACECode

[![Stars](https://img.shields.io/github/stars/shaohaozhi286/acecode?style=flat-square)](https://github.com/shaohaozhi286/acecode/stargazers)
[![Forks](https://img.shields.io/github/forks/shaohaozhi286/acecode?style=flat-square)](https://github.com/shaohaozhi286/acecode/network/members)
[![Issues](https://img.shields.io/github/issues/shaohaozhi286/acecode?style=flat-square)](https://github.com/shaohaozhi286/acecode/issues)
[![Last Commit](https://img.shields.io/github/last-commit/shaohaozhi286/acecode?style=flat-square)](https://github.com/shaohaozhi286/acecode/commits)

[English](README.md) | 中文

ACECode 是一个面向终端的 C++17 AI 编程助手。它提供基于 FTXUI 的 TUI、GitHub Copilot、OpenAI 兼容接口与 Anthropic 支持、用于仓库工作的工具调用、会话持久化、daemon HTTP/WebSocket API、React Web UI，以及可选的桌面壳。

![ACE Code](https://2017studio.oss-cn-beijing.aliyuncs.com/acecode.jpg)

## 功能特性

- 交互式终端 UI，支持流式响应、输入历史、聊天滚动、Markdown 渲染和实时工具进度。
- 多轮 agent loop，支持工具调用、权限确认、取消、压缩、回退和会话恢复。
- 支持 GitHub Copilot Device Flow 登录、OpenAI 兼容 Chat Completions 端点和 Anthropic Messages API。
- 内置仓库工具，可执行 shell、读写编辑文件、搜索、glob 匹配、向用户提问、使用技能、读取记忆，并可选启用联网搜索。
- Daemon 模式提供 HTTP/WebSocket 端点、非 loopback token 鉴权，以及内置 React/Vite Web UI。
- 可选桌面壳可管理多个 workspace，并为每个活跃 workspace 运行独立 daemon 进程。
- 支持 macOS、Linux 和 Windows 跨平台构建。

## 从 npm 安装

全局安装终端 CLI：

```bash
npm install -g acecode
acecode
```

如果只想安装到当前项目，运行 `npm install acecode`，再用 `npx acecode`
启动。桌面壳可通过 `npm install -g @aceagent/desktop` 安装。

## 从源码构建

### 环境要求

- CMake 3.20 或更新版本。
- Ninja 或其他 CMake 支持的 generator。
- C++17 编译器：MSVC 2019+、GCC 9+ 或 Clang 10+。
- 支持 submodule 的 Git。
- [vcpkg](https://github.com/microsoft/vcpkg)。
- 构建内置 Web UI 时需要 `pnpm`。

将 `<triplet>` 设置为以下受支持的 vcpkg triplet 之一：

| 平台 | Triplet |
| --- | --- |
| Linux x64 | `x64-linux` |
| Linux arm64 | `arm64-linux` |
| Windows x64 | `x64-windows-static` |
| Windows x64（WinLibs/MinGW） | `x64-mingw-static` |
| macOS x64 | `x64-osx` |
| macOS arm64 | `arm64-osx` |

### Web UI

CMake 会从 `web/dist/` 嵌入前端资源。如果希望 daemon 提供完整浏览器 UI，请先构建 Web UI，再运行 `cmake -S ...`。

```bash
cd web
pnpm install
pnpm test
pnpm build
cd ..
```

如果 CMake configure 时缺少 `web/dist/`，构建会嵌入最小 fallback 页面，daemon API 仍可构建，但不会包含完整 Web UI。详见 [web/README.md](web/README.md)。

### 配置与构建

Preset 构建要求 `VCPKG_ROOT` 指向你的 vcpkg checkout。常用 Release 流程是：

```bash
git submodule update --init --recursive

cmake --preset linux-x64-release
cmake --build --preset linux-x64-release
```

普通 presets 使用 `<platform>-<arch>-<config>` 命名，其中 `<config>` 是
`release` 或 `debug`：`windows-x64`、`linux-x64`、`linux-arm64`、
`macos-x64` 和 `macos-arm64`。桌面壳变体使用
`<platform>-<arch>-desktop-<config>`，例如
`macos-arm64-desktop-debug`。

Debug 构建：

```bash
cmake --preset linux-x64-debug
cmake --build --preset linux-x64-debug
```

如果要在 Windows 上使用自包含的 GCC/Ninja/GDB 环境，解压 WinLibs，并让
`ACECODE_WINLIBS_ROOT` 指向其中的 `mingw64` 目录。这个 preset 会固定整套
构建工具，不会再从 `PATH` 中混入其他编译器：

```powershell
$env:ACECODE_WINLIBS_ROOT = 'C:\tools\winlibs\mingw64'
$env:VCPKG_ROOT = 'C:\vcpkg'
$env:PATH = "$env:ACECODE_WINLIBS_ROOT\bin;$env:PATH"

& "$env:ACECODE_WINLIBS_ROOT\bin\cmake.exe" --preset windows-x64-winlibs-debug
& "$env:ACECODE_WINLIBS_ROOT\bin\cmake.exe" --build --preset windows-x64-winlibs-debug
& "$env:ACECODE_WINLIBS_ROOT\bin\gdb.exe" build/windows-x64-winlibs-debug/acecode.exe
```

该 preset 还会生成 `compile_commands.json`，为目标和 host 依赖都使用
MinGW vcpkg triplet，并将构建并行数限制为 8，以提高最小化环境下的稳定性。
WinToast 依赖 Windows SDK，因此纯 MinGW 构建会
使用通知 stub；普通 Windows presets 仍保留原生 WinToast 通知。

等价手动配置：

```bash
git submodule update --init --recursive

cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=<triplet> \
  -DVCPKG_OVERLAY_PORTS=$PWD/ports \
  -DBUILD_TESTING=ON

cmake --build build --config Release
```

单配置 Ninja 构建下，主二进制是 `build/acecode`。

### 测试

```bash
cmake --build build --target acecode_unit_tests
ctest --test-dir build --output-on-failure
```

使用 presets 时，先构建对应测试 target，再运行 test preset：

```bash
cmake --build --preset linux-x64-release-tests
ctest --preset linux-x64-release-tests
```

Debug 测试 preset 同样按这个模式命名，例如 `linux-x64-debug-tests`。

单元测试位于 `tests/`，并链接到 `acecode_testable` object library。TUI 较重的入口代码不进入测试 target，除非逻辑已经拆成可复用 helper。

### 桌面构建

这里使用上面列出的同一组受支持 presets/triplet。如果桌面壳需要完整内置 Web UI，请先构建 [web/](web)，再配置 CMake。

```bash
cmake --preset linux-x64-desktop-release
cmake --build --preset linux-x64-desktop-release
```

Debug 桌面构建使用对应的 `*-desktop-debug` preset。

等价手动配置：

```bash
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=<triplet> \
  -DVCPKG_OVERLAY_PORTS=$PWD/ports \
  -DACECODE_BUILD_DESKTOP=ON

cmake --build build --target acecode-desktop
```

Linux 构建桌面 target 前需要安装 WebKitGTK 开发包，例如 Ubuntu 24.04 上的 `sudo apt install libwebkit2gtk-4.1-dev`。运行时也需要系统 WebKitGTK 库，例如 Ubuntu 上的 `libwebkit2gtk-4.1-0`。

在 macOS 上，这个 target 会产出 `build/ACECode.app`；在 Windows 和 Linux 上会产出 flat layout，`acecode-desktop` 与配套的 `acecode` daemon 可执行文件位于同一个 build 目录。

### Windows 注意事项

Windows 构建会启用 UTF-8 编译选项；使用 vcpkg toolchain 且未指定 triplet 时，默认使用静态 vcpkg triplet。Windows TLS 行为要求 libcurl 8.14 或更新版本。

## 快速开始

从 [Releases](https://github.com/shaohaozhi286/acecode/releases) 下载预编译二进制，然后运行：

```bash
./acecode configure
cd /path/to/your/project
./acecode
```

配置向导会创建 `~/.acecode/config.json`。你可以选择 GitHub Copilot 进行 Device Flow 登录，也可以选择 OpenAI 兼容端点并填写 `base_url`、`api_key` 和模型名称。

TUI 启动后，输入需求并回车：

```text
重构 session serializer，并补充聚焦的单元测试
```

只读工具通常会自动运行。文件写入、文件编辑和 shell 命令会请求确认，除非当前权限模式或规则允许直接执行。

## 运行模式

### 终端 CLI

```bash
./acecode                     # 在当前目录启动新的 TUI 会话
./acecode -r                  # 启动 TUI 并立即打开会话选择器
./acecode --resume            # 恢复当前项目最近一次会话
./acecode --resume <id>       # 恢复指定会话
./acecode configure           # 运行配置向导
./acecode --alt-screen        # 本次启动强制使用全屏 alternate screen 渲染
./acecode --yolo              # 跳过权限确认；等同于 --dangerous
```

普通 TUI 模式需要交互式 TTY。建议从希望 agent 操作的项目根目录启动；脚本和管道请使用 print 模式。

### 无头 Print 模式

`-p` / `--print` 会执行一个非交互 agent turn，把结果写到 stdout，并持久化为可恢复的普通会话：

```bash
./acecode -p "解释这个仓库"
git diff | ./acecode -p "审查这份 diff"
./acecode -p --output-format stream-json "实现这个改动"
./acecode -p --model fast --permission-mode plan --max-turns 8 "制定计划"
```

Print 模式默认值是确定的：新会话使用配置的默认模型（恢复会话会沿用其已保存模型，除非显式传入 `--model`）、权限模式为 `default`、不限制 turn、启用当前配置下实际可用的全部系统工具，同时不暴露任何 Skill 和 MCP。普通无头模式下，需要交互确认的工具会被拒绝；只有明确需要时才选择合适权限模式或安全风险更高的 `--yolo`。

能力列表使用区分大小写的精确名称，支持逗号分隔和重复传参：

```bash
./acecode -p --disable-tools bash,file_write "只分析，不改文件"
./acecode -p --enable-skills code-review --enable-mcp github "审查这个 PR"
./acecode -p --enable-mcp github,linear --enable-mcp browser "处理这个问题"
```

- `--disable-tools <names>`：移除点名的 ACECode 系统工具。
- `--enable-skills <names>`：只启用点名的已安装 Skill。
- `--enable-mcp <names>`：只启动点名且未被全局禁用的已配置 MCP server。

未知或全局不可用的名称会在模型 turn 前失败，退出码为 64，并列出本次可用名称。此前依赖隐式 Skill/MCP 能力的脚本现在必须显式启用。运行 `acecode -p --help` 可查看输出格式、会话续接和完整参数。

### Daemon 与 Web UI

```bash
./acecode daemon --foreground
./acecode daemon start
./acecode daemon status
./acecode daemon stop
```

Daemon 默认在 `http://127.0.0.1:28080` 提供 API 和内置 Web UI。可在 `~/.acecode/config.json` 中配置 `web.bind`、`web.port` 和 `web.static_dir`。

运行时文件位于 `<data_dir>/run/`。每次 daemon 启动都会写入 token 文件；loopback 客户端可以省略 token，非 loopback 客户端必须传入 `X-ACECode-Token` 请求头或 `?token=` 查询参数。协议详见 [docs/daemon-api.md](docs/daemon-api.md)。

### Windows 服务

在 Windows 上，ACECode 可以把 daemon 安装为 SCM 服务：

```powershell
acecode service install
acecode service start
acecode service status
acecode service stop
acecode service uninstall
```

服务模式使用平台服务数据目录，而不是普通用户数据目录，因此它与 TUI 模式的会话和配置相互独立。

### 桌面壳

可选的 `acecode-desktop` target 会把 Web UI 包装为原生桌面壳。它可以通过共享 daemon 进程跟踪多个 workspace。构建时传入 `-DACECODE_BUILD_DESKTOP=ON`；Linux 上通过 `webview/webview` 使用 WebKitGTK。当前模型见 [docs/desktop-shell/multi-workspace.md](docs/desktop-shell/multi-workspace.md)。

## TUI 使用

### 斜杠命令

| 命令 | 用途 |
| --- | --- |
| `/help` | 显示命令帮助。 |
| `/clear` | 清空当前对话。 |
| `/model` | 查看或切换当前模型配置。 |
| `/config` | 显示当前 provider、模型、上下文窗口和权限模式。 |
| `/tokens` | 显示本会话 token 用量。 |
| `/compact` | 压缩较长的对话历史。 |
| `/resume` | 打开会话选择器。 |
| `/rewind` | 回退到之前的用户轮次。 |
| `/checkpoint` | `/rewind` 的别名。 |
| `/mcp` | 管理 MCP servers 和工具。 |
| `/skills` | 列出、调用或重新加载已安装技能。 |
| `/memory` | 管理持久化用户记忆。 |
| `/init` | 为当前目录生成或改进项目指令。 |
| `/history` | 列出或清理当前工作目录的输入历史。 |
| `/proxy` | 查看、刷新或覆盖 LLM/API 请求使用的 HTTP 代理。 |
| `/websearch` | 查看或切换联网搜索 backend。 |
| `/title` | 设置或显示终端标题。 |
| `/page-step` | 在默认单行 PgUp/PgDn 和按页滚动之间切换。 |
| `/models` | 检查内置 models.dev registry。 |
| `/exit` | 退出 ACECode。 |

已安装技能也会按技能名注册斜杠命令。

### 内置工具

| 工具 | 默认权限 | 用途 |
| --- | --- | --- |
| `bash` | 请求确认 | 在项目上下文中执行 shell 命令。 |
| `file_read` | 自动执行 | 读取文件，支持行范围。 |
| `file_write` | 请求确认 | 创建或覆盖文件。 |
| `file_edit` | 请求确认 | 应用精确文本编辑。 |
| `grep` | 自动执行 | 用正则搜索文件内容。 |
| `glob` | 自动执行 | 按 glob 模式查找路径。 |
| `task_complete` | 自动执行 | 可选的显式任务完成信号。 |
| `AskUserQuestion` | UI 中确认 | 向用户提出结构化追问。 |
| `skills_list`, `skill_view` | 自动执行 | 发现并加载技能指令。 |
| `memory_read`, `memory_write` | 自动 / 受限写入 | 读取和更新持久化记忆。 |
| `web_search` | 自动执行 | 在配置启用时执行联网搜索。 |

MCP servers 可以在运行时添加更多工具。

## 配置要点

主配置文件是 `~/.acecode/config.json`。用户数据还包括 Copilot token、会话元数据、输入历史、记忆文件、daemon 运行时文件和 `~/.acecode/` 下的状态文件。

重要配置区域：

| 区域 | 用途 |
| --- | --- |
| `provider`, `openai`, `copilot` | 旧式 provider 选择和端点/模型设置。 |
| `openai.stream_timeout_ms` | HTTP provider 流式请求超时，单位毫秒；默认 `666000`，慢网关可调大。 |
| `saved_models`, `default_model_name` | 命名模型配置和默认模型；OpenAI 与 Anthropic 条目也可以设置 `stream_timeout_ms` 和 `request_headers`。 |
| `context_window`, `models_dev` | 上下文窗口解析和内置 models.dev 查询行为。 |
| `skills`, `memory`, `project_instructions` | 可选上下文来源及其限制。 |
| `agent_loop.max_iterations` | 单次 agent 回合硬上限；`0` 或省略表示无限制。 |
| `daemon`, `web` | Daemon 心跳、服务、bind、port 和静态资源设置。 |
| `network` | 系统/手动代理、代理探测和 TLS 选项。 |
| `web_search` | 联网搜索工具开关和 backend 选择。 |
| `tui.alt_screen_mode` | 终端渲染模式；`auto` 默认全屏，`never` 恢复 terminal-output 模式。 |
| `desktop.notifications` | 桌面壳通知行为。 |
| `mcp_servers` | Stdio、SSE 或 Streamable HTTP MCP server 定义。 |

模型选择细节见 [docs/model-context-resolution.md](docs/model-context-resolution.md)。

## 参考文档

- [ARCHITECTURE.md](ARCHITECTURE.md)：稳定的运行时与源码归属地图。
- [AGENTS.md](AGENTS.md)：贡献者和 coding agent 规则。
- [CLAUDE.md](CLAUDE.md)：当前实现记忆。
- [docs/user-manual.md](docs/user-manual.md)：用户工作流细节。
- [docs/daemon-api.md](docs/daemon-api.md)：daemon HTTP/WebSocket API。
- [docs/hooks.md](docs/hooks.md)：Codex 兼容生命周期钩子、信任审核和设置页管理。
- [docs/model-context-resolution.md](docs/model-context-resolution.md)：模型配置和上下文窗口解析。
- [docs/skills.md](docs/skills.md)：技能编写和使用。
- [docs/skills-implementation.md](docs/skills-implementation.md)：技能运行时实现说明。
- [docs/desktop-shell/multi-workspace.md](docs/desktop-shell/multi-workspace.md)：桌面多 workspace 行为。

## 许可证

重新分发二进制或内置资源前，请先确认仓库许可证。
