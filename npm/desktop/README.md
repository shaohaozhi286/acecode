# @aceagent/desktop

ACECode Desktop — ACECode AI coding agent 的多 workspace 桌面外壳
（系统托盘、OS 通知、每个 workspace 独立 daemon）。

## 安装与启动

```bash
npm install -g @aceagent/desktop
acecode-desktop
```

`acecode-desktop` 命令会以独立进程启动图形界面并立即返回。

## 平台要求

| 平台 | 说明 |
|---|---|
| Windows x64 | 需要 WebView2 运行时（Win10/11 一般已自带；缺失时自动降级为浏览器模式） |
| macOS x64 / arm64 | 系统 WebKit，无额外依赖 |
| Linux x64 / arm64 | 需要 `libwebkit2gtk-4.1`（如 `sudo apt install libwebkit2gtk-4.1-0`） |

真实二进制由 `@aceagent/<os>-<cpu>` 平台包按平台自动安装（`optionalDependencies`），
其中同时包含 daemon 主程序 `acecode`，桌面壳按同目录关系自动找到它。
安装时**不要**使用 `--omit=optional` / `--no-optional`。

终端 CLI 版本见 [`@aceagent/acecode`](https://www.npmjs.com/package/@aceagent/acecode)。

## 许可

版权所有。二进制可免费使用；源码许可见仓库
<https://github.com/shaohaozhi286/acecode>。
