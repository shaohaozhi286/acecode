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

基于 C++17 的终端 AI 编程助手。通过 [FTXUI](https://github.com/ArthurSonzogni/FTXUI) 提供交互式 TUI 界面，支持 OpenAI 兼容 API 和 GitHub Copilot 的tool-use。

![ACE Code](https://2017studio.oss-cn-beijing.aliyuncs.com/acecode.jpg)
## 特性

- **交互式 TUI** -- 基于 FTXUI 的终端界面，支持流式响应、输入历史和鼠标操作
- **Agent 循环** -- 多轮对话，自动调用工具并在执行前请求用户确认
- **双 Provider 支持**
  - **OpenAI 兼容** -- 接入任意实现了 OpenAI Chat Completions 接口的 API（本地大模型、云端端点等）
  - **GitHub Copilot** -- Device Flow OAuth 认证，令牌持久化
- **内置工具**
  - `Bash` -- 执行 Shell 命令
  - `FileRead` / `FileWrite` / `FileEdit` -- 读取、创建、修补文件
  - `Grep` -- 正则搜索文件内容
  - `Glob` -- 按通配符匹配文件路径
- **跨平台** -- 支持 Windows 和 Linux/macOS

## 环境要求

- CMake >= 3.20
- C++17 编译器（MSVC 2019+、GCC 9+、Clang 10+）
- Git
- [vcpkg](https://github.com/microsoft/vcpkg)

## 构建

```bash
# 拉取本地 overlay port 所依赖的 FTXUI 子模块
git submodule update --init --recursive

# 为目标 triplet 安装依赖
<vcpkg-root>/vcpkg install \
  cpr \
  nlohmann-json \
  ftxui \
  --triplet <triplet> \
  --overlay-ports=$PWD/ports

# 配置（根据实际情况调整 vcpkg 路径和 triplet）
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg-root>/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=<triplet> \
  -DVCPKG_OVERLAY_PORTS=$PWD/ports

# 编译
cmake --build build --config Release
```

常用 triplet：

- `x64-linux`
- `arm64-linux`
- `x64-windows`
- `x64-osx`
- `arm64-osx`

使用 Ninja 单配置生成器时，可执行文件输出到 `build/acecode`；如果使用多配置生成器，则输出到 `build/<config>/`。

## GitHub Actions 打包

仓库已包含 `.github/workflows/package.yml`，会自动构建并上传以下平台产物：

- Linux x64
- Linux arm64
- Windows x64
- macOS x64
- macOS arm64

你可以在 **Actions > package > Run workflow** 手动触发，也可以在 Pull Request、推送到 `main` 或打 `v*` 版本标签时自动运行。

### Windows 注意事项

Windows 下 `cpr` 依赖的 libcurl 版本必须 **>= 8.14** 才能正确处理 TLS 证书。CMake 构建时会自动检测，如果版本过低会提前报错。请确保你的 vcpkg 仓库足够新。

## 配置

配置文件路径：`~/.acecode/config.json`（首次运行时自动创建）。

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

| 字段 | 说明 |
|------|------|
| `provider` | `"copilot"` 或 `"openai"` |
| `openai.base_url` | API 端点地址 |
| `openai.api_key` | API 密钥 |
| `openai.model` | 请求的模型名称 |
| `copilot.model` | Copilot 模型名称（默认 `gpt-4o`） |

## 使用

```bash
./build/Release/acecode
```
 

## 许可证

MIT
