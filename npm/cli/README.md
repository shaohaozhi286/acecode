# @acecode/cli

ACECode — AI coding agent，单二进制自带终端 TUI、后台 daemon 与浏览器 Web UI。

## 安装

```bash
npm install -g @acecode/cli
```

安装后可直接使用 `acecode` 命令：

```bash
acecode              # 终端 TUI
acecode -p "prompt"  # headless print 模式
acecode daemon start # 后台 daemon + Web UI (http://localhost:28080)
```

## 工作方式

本包只包含一个很小的 Node 启动器；真实的原生二进制按平台放在
`@acecode/<os>-<cpu>` 平台包中，通过 `optionalDependencies` 在安装时自动选择：

| 平台 | 包 |
|---|---|
| Linux x64 | `@acecode/linux-x64` |
| Linux arm64 | `@acecode/linux-arm64` |
| macOS x64 | `@acecode/darwin-x64` |
| macOS arm64 | `@acecode/darwin-arm64` |
| Windows x64 | `@acecode/win32-x64` |

注意：

- 安装时**不要**使用 `--omit=optional` / `--no-optional`，否则平台二进制不会被安装。
- 旧发行版（glibc < 2.31）请改用 GitHub Releases 中的 `acecode-linux-old-*` 压缩包：
  <https://github.com/shaohaozhi286/acecode/releases>
- 不在上表中的平台暂无预编译产物，同样请到 GitHub Releases 查看或自行构建。

## 桌面版

图形界面外壳见 [`@acecode/desktop`](https://www.npmjs.com/package/@acecode/desktop)。

## 许可

版权所有。二进制可免费使用；源码许可见仓库
<https://github.com/shaohaozhi286/acecode>。
