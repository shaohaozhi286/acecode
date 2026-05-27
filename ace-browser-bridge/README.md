# ace-browser-bridge

`ace-browser-bridge` 是 ACECode 的浏览器插件交付物。`ace-browser-host` 是独立的本地 C++ CLI/daemon 项目，可执行文件名为 `ace-browser-host.exe`，默认 daemon 地址为 `127.0.0.1:52007`。

插件负责：

- 与 `ace-browser-host` daemon 建立握手和轮询连接。
- 维护 managed browser session、owned/adopted tab 和 Chrome tab group 状态色。
- 注入 `content/virtual-cursor.js`，提供 snapshot、`@e` 元素引用、目标解析、DOM 动作、operation overlay 和显式用户输入 block。
- 通过 Chrome Debugger API 执行 CDP pointer、typing、network capture 和 PDF 导出。
- 通过 popup 显示连接状态、最近错误，并提供紧急释放页面接管提示。

## 加载到 Chrome

1. 打开 `chrome://extensions`。
2. 启用 `Developer mode`。
3. 点击 `Load unpacked`。
4. 选择目录：`C:\Users\shao\acecode\ace-browser-bridge`。
5. 启动 `ace-browser-host.exe serve --json --port 52007`。
6. 打开普通 `http://` 或 `https://` 页面，插件会连接本地 daemon。

Chrome 内部页无法注入扩展脚本，例如 `chrome://extensions`、`chrome://newtab` 和 Chrome Web Store。测试时使用普通网页。

## 文件

- `manifest.json`：MV3 插件声明、权限、service worker 和 content script。
- `service_worker.js`：daemon 连接、session/tab 管理、CDP、network、截图/PDF 和 action dispatch。
- `content/virtual-cursor.js`：页面 snapshot、`@e` refs、DOM fallback、operation overlay、pointer debug visualization。
- `popup.html`、`popup.css`、`popup.js`：插件状态 UI 和紧急释放入口。
- `scripts/smoke.ps1`：静态 smoke 检查和 JavaScript 语法检查。

## Smoke 检查

```powershell
powershell -ExecutionPolicy Bypass -File ace-browser-bridge/scripts/smoke.ps1
```

该脚本检查 manifest 权限、默认端口、协议握手、CDP 输入路径、operation overlay、显式用户输入 block、pointer debug visualization、跨域 iframe 错误路径，并在本机有 Node.js 时运行 `node --check`。
