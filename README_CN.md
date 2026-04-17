```ansi
░█▀█░█▀▀░█▀▀░
░█▀█░█░░░█▀▀░
░▀░▀░▀▀▀░▀▀▀░
```
[![Stars](https://img.shields.io/github/stars/shaohaozhi286/acecode?style=flat-square)](https://github.com/shaohaozhi286/acecode/stargazers)
[![Forks](https://img.shields.io/github/forks/shaohaozhi286/acecode?style=flat-square)](https://github.com/shaohaozhi286/acecode/network/members)
[![Issues](https://img.shields.io/github/issues/shaohaozhi286/acecode?style=flat-square)](https://github.com/shaohaozhi286/acecode/issues)
[![Last Commit](https://img.shields.io/github/last-commit/shaohaozhi286/acecode?style=flat-square)](https://github.com/shaohaozhi286/acecode/commits)

[English](README.md) | 中文

**ACECode** 是一个基于 C++17 的终端 AI 编程助手。它运行在你的 shell 中，可对接 OpenAI 兼容接口或 GitHub Copilot，通过工具调用读写项目文件、执行 shell 命令、搜索代码——所有交互都在 [FTXUI](https://github.com/ArthurSonzogni/FTXUI) 驱动的 TUI 界面中完成。

![ACE Code](https://2017studio.oss-cn-beijing.aliyuncs.com/acecode.jpg)

## 特性

- **交互式 TUI** —— 流式响应、输入历史、鼠标支持
- **多轮 Agent 循环** —— 自动调用工具，每个工具执行前请求确认
- **双 Provider 支持**
  - **OpenAI 兼容** —— 任何实现 OpenAI Chat Completions 接口的端点（本地大模型、云端、代理等）
  - **GitHub Copilot** —— Device Flow OAuth 登录，令牌持久化、自动刷新
- **内置工具** —— `Bash`、`FileRead`、`FileWrite`、`FileEdit`、`Grep`、`Glob`
- **会话持久化** —— 通过 Session ID 恢复任意历史对话
- **跨平台** —— 提供 Linux、macOS、Windows 二进制包

---

## Quick Start（快速开始）

如果你只想试用 ACECode，可直接到 [Releases](https://github.com/shaohaozhi286/acecode/releases) 页面下载预编译二进制（Linux x64/arm64、Windows x64、macOS x64/arm64），然后：

```bash
# 1. 首次配置 —— 选择 Provider 和模型
./acecode configure

# 2. 进入项目目录并启动
cd /path/to/your/project
./acecode
```

首次启动会引导你完成：

- 选择 **GitHub Copilot**（推荐，无需 API Key）或 **OpenAI 兼容**端点
- 选择 Copilot 时：浏览器内一次性 Device Login，令牌自动保存到 `~/.acecode/`
- 选择 OpenAI 兼容时：填写 `base_url`、`api_key`、模型名称

随后在 TUI 输入框中描述你的需求并回车：

```
> 把 src/main.cpp 中的 CLI 解析逻辑抽取到独立文件
```

Agent 会自动规划任务、调用工具（写入/执行类操作会请求确认），并把过程流式返回。

---

## How to Use（如何使用）

### 命令行参数

```bash
./acecode                     # 在当前目录开启全新会话
./acecode --resume            # 恢复最近一次会话
./acecode --resume <id>       # 通过 Session ID 恢复指定会话
./acecode configure           # 运行交互式配置向导
./acecode --dangerous         # 跳过所有权限确认（请谨慎使用）
```

> ACECode 需要交互式 TTY，stdin/stdout 被重定向时会拒绝启动。

### TUI 内的斜杠命令

| 命令         | 说明                              |
|--------------|-----------------------------------|
| `/help`      | 列出所有可用命令                  |
| `/clear`     | 清空对话历史                      |
| `/model`     | 查看或切换当前模型                |
| `/config`    | 显示当前配置                      |
| `/cost`      | 显示 token 用量与成本估算          |
| `/compact`   | 压缩对话历史以节省 token           |
| `/resume`    | 打开会话选择器恢复历史会话         |
| `/exit`      | 退出 ACECode                      |

### 内置工具

由模型自主决定调用哪些工具。默认情况下，**只读工具自动执行**，**写入/执行类工具会请求确认**。

| 工具         | 用途                              |
|--------------|-----------------------------------|
| `Bash`       | 执行 Shell 命令                   |
| `FileRead`   | 读取文件内容                      |
| `FileWrite`  | 创建或覆盖文件                    |
| `FileEdit`   | 对文件进行精准编辑                |
| `Grep`       | 跨文件正则搜索                    |
| `Glob`       | 按通配符匹配文件                  |

### 配置

配置文件路径：`~/.acecode/config.json`（首次运行或执行 `acecode configure` 时自动创建）。

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

| 字段              | 说明                                |
|-------------------|-------------------------------------|
| `provider`        | `"copilot"` 或 `"openai"`           |
| `openai.base_url` | API 端点地址                        |
| `openai.api_key`  | API 密钥                            |
| `openai.model`    | 请求的模型名称                      |
| `copilot.model`   | Copilot 模型名称（默认 `gpt-4o`）   |

会话按项目目录持久化到 `.acecode/projects/<cwd_hash>/`。

### 小贴士

- 在项目根目录启动 ACECode，路径校验会以该目录为根。
- 长会话接近上下文上限时使用 `/compact` 压缩历史。
- `--dangerous` 适合在沙盒环境（容器、VM）中使用，但会跳过全部安全确认。

---

## How to Build（如何从源码构建）

如果你需要从源码构建（例如二次开发或为未支持的平台编译）：

### 环境要求

- CMake **>= 3.20** 和 Ninja
- C++17 编译器：MSVC 2019+、GCC 9+ 或 Clang 10+
- Git
- [vcpkg](https://github.com/microsoft/vcpkg)

### 步骤

```bash
# 1. 拉取代码（含子模块，本地 FTXUI overlay port 依赖它）
git clone --recursive https://github.com/shaohaozhi286/acecode.git
cd acecode
# （如已克隆：git submodule update --init --recursive）

# 2. 通过 vcpkg + 本地 overlay-ports 安装依赖
<vcpkg-root>/vcpkg install \
  cpr nlohmann-json ftxui \
  --triplet <triplet> \
  --overlay-ports=$PWD/ports

# 3. 配置
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=<triplet> \
  -DVCPKG_OVERLAY_PORTS=$PWD/ports

# 4. 编译
cmake --build build --config Release
```

常用 triplet：`x64-linux`、`arm64-linux`、`x64-windows`、`x64-windows-static`（Windows 下使用 MSVC 静态运行库时选用）、`x64-osx`、`arm64-osx`。

可执行文件输出到 `build/acecode`（Ninja 单配置）或 `build/<config>/acecode`（多配置生成器）。

### Windows 注意事项

Windows 下 `cpr` 依赖的 libcurl 必须 **>= 8.14** 才能正确处理 TLS 证书。CMake 构建会自动检测并提前报错，请确保 vcpkg 仓库足够新。

### CI / 打包

`.github/workflows/package.yml` 会为 Linux x64/arm64、Windows x64、macOS x64/arm64 自动构建并上传产物。会在 PR、推送到 `main`、打 `v*` 版本标签时触发，也可以在 **Actions > package > Run workflow** 手动触发。

---

## 许可证

MIT
