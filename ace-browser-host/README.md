# ace-browser-host

`ace-browser-host` 是 ACECode 浏览器工具使用的本地 C++ CLI/daemon。Windows 可执行文件名为 `ace-browser-host.exe`，Unix-like 平台为 `ace-browser-host`。文本按原生 UTF-8 处理。

浏览器插件交付物是独立项目 `ace-browser-bridge`。默认 daemon 地址为 `127.0.0.1:52007`。

## 命令

```bash
ace-browser-host.exe status --json
ace-browser-host.exe command --json
ace-browser-host.exe screenshot --json --session <name> --output <path>
ace-browser-host.exe serve --json
ace-browser-host.exe shutdown --json
```

`command --json` 从 stdin 读取：

```json
{"session":"acecode-demo","action":"snapshot","args":{}}
```

所有响应都使用统一 envelope：

```json
{"ok":true,"data":{}}
{"ok":false,"error":{"code":"daemon_not_running","message":"ace-browser-host daemon is not running"}}
```

`status --json` 也使用 envelope。daemon 不可连接时返回 `ok:true` 且 `running:false`，方便 ACECode 展示健康状态，而不是把 daemon 未启动视为 CLI 执行失败。

ACECode 的内置 `browser_*` 工具会在调用时自动检查并后台启动 `ace-browser-host serve --json --port 52007`。手动运行 `serve` 主要用于独立调试 host、插件或 CLI 协议。

## Daemon HTTP Surface

daemon 只监听 loopback，并要求本地调用头：

- CLI 端点：`X-Ace-Browser-Host: 1`
- 插件端点：`X-Ace-Browser-Bridge: extension`

端点：

- `GET /status`
- `POST /command`
- `POST /plugin/hello`
- `POST /plugin/poll`
- `POST /plugin/result`
- `POST /shutdown`

`/plugin/hello` 由 `ace-browser-bridge` 上报 `protocol_version`、插件版本、浏览器类型和 capabilities。当前协议版本为 `0.1`；如果插件协议与 CLI daemon 不兼容，页面动作会返回：

```json
{"ok":false,"error":{"code":"version_mismatch","message":"..."}}
```

截图和 PDF 等二进制结果由插件返回 base64 给 CLI，CLI 写入本地文件后移除 `data` 字段，只把路径、MIME type 和大小返回给 ACECode。
