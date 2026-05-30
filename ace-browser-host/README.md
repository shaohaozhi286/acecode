# ace-browser-host

`ace-browser-host` 是 ACECode 浏览器能力使用的本地 C++ CLI/daemon。Windows 可执行文件名为 `ace-browser-host.exe`，Unix-like 平台为 `ace-browser-host`。文本按原生 UTF-8 处理。

浏览器插件交付物是独立项目 `ace-browser-bridge`。默认 daemon 地址为 `127.0.0.1:52007`。

## 命令

```bash
ace-browser-host.exe start --json
ace-browser-host.exe ensure-ready --json [--timeout-ms <ms>] [--no-launch-browser]
ace-browser-host.exe status --json
ace-browser-host.exe open --json --session <name> --url <url> [--new-tab] [--group-title <title>] [--timeout-ms <ms>]
ace-browser-host.exe find-tab --json --session <name> (--url <text>|--tab-id <id>|--active)
ace-browser-host.exe navigate --json --session <name> --operation <goto|back|forward|reload> [--url <url>] [--timeout-ms <ms>]
ace-browser-host.exe read-page --json --session <name> [--mode summary|elements|focused|changed]
ace-browser-host.exe wait --json --session <name> --condition <condition> [--target <ref|selector>] [--text <text>] [--timeout-ms <ms>]
ace-browser-host.exe assert --json --session <name> --condition <condition> [--target <ref|selector>] [--text <text>] [--value <text>] [--url <text>] [--method <GET|POST>] [--status-class <2xx|3xx|4xx|5xx>] [--timeout-ms <ms>]
ace-browser-host.exe batch --json --session <name> [--steps-file <path>]
ace-browser-host.exe block-input --json --session <name> [--watchdog-ms <ms>] [--message <text>]
ace-browser-host.exe unblock-input --json --session <name>
ace-browser-host.exe click --json --session <name> (--target <ref>|--target-text <text>|--locator <json>|--x <n> --y <n>)
ace-browser-host.exe fill --json --session <name> (--target <ref>|--locator <json>|--role <role> --name <name>) --value <text>
ace-browser-host.exe type --json --session <name> (--target <ref>|--locator <json>) [--text <text>] [--submit]
ace-browser-host.exe hover --json --session <name> (--target <ref>|--locator <json>)
ace-browser-host.exe drag --json --session <name> --from <ref> (--to <ref>|--offset <x>,<y>)
ace-browser-host.exe scroll --json --session <name> --delta-y <n>
ace-browser-host.exe evaluate --json --session <name> --code <javascript>
ace-browser-host.exe network --json --session <name> --cmd <start|stop|list|detail>
ace-browser-host.exe screenshot --json --session <name> --output <path> [--target <ref|selector>|--locator <json>|--attachment-ref <ref>|--attachment-url <url>]
ace-browser-host.exe save-pdf --json --session <name> [--file-name <name>]
ace-browser-host.exe list-tabs --json --session <name>
ace-browser-host.exe close-session --json --session <name>
ace-browser-host.exe command --json
ace-browser-host.exe serve --json
ace-browser-host.exe shutdown --json
```

`start --json` 会在 daemon 未运行时后台启动 `serve --json --port 52007`，然后轮询 `status`。

`ensure-ready --json` 是浏览器工具的推荐入口。它会确保 daemon 运行；如果扩展未连接或连接过期，会打开 `http://127.0.0.1:52007/wake` 唤醒默认浏览器和 `ace-browser-bridge` 扩展，然后在超时内等待 `ready=true`。调试时可加 `--no-launch-browser` 只检查和等待，不主动打开浏览器。

`command --json` 是底层入口，从 stdin 读取：

```json
{"session":"acecode-demo","action":"snapshot","args":{}}
```

其他子命令只是把结构化 argv 转成同样的 `{session, action, args}` 请求。

`block-input` 会在当前 session 的页面上显示“AI 正在操作浏览器”的明显遮罩并拦截用户输入；如果 session 还没有 tab，会先记录 block 状态，后续 `open` 或 `find-tab` 绑定 tab 后自动应用。任务完成后应单独执行 `unblock-input` 清理遮罩和输入拦截。`--watchdog-ms` 是兜底超时，默认 300000 毫秒。

`assert` 用于独立验证页面状态，失败返回 `assertion_failed` 并携带 `observed`。改动类动作可通过 `--args-json` 传入 `expect` 子句，把动作和确认折进一次调用：

```bash
ace-browser-host.exe click --json --session demo --target @e15 --args-json "{\"expect\":{\"condition\":\"text_equals\",\"target\":\"#status\",\"text\":\"Saved\",\"timeout_ms\":5000}}"
ace-browser-host.exe assert --json --session demo --condition request_completed --url "/api/save" --status-class 2xx --timeout-ms 10000
```

目标元素可以用结构化 locator 描述,避免业务页面里同名按钮点错：

```bash
ace-browser-host.exe click --json --session demo --locator "{\"role\":\"button\",\"name\":\"Save\",\"within\":{\"role\":\"row\",\"name\":\"BUG-123\"}}"
```

`read-page` 会返回元素 value/options/disabled/ARIA/actionable/context/stable selector,以及页面 focused/viewport/attachments。`attachments` 使用 `@a` ref,可通过 `screenshot --attachment-ref @a1 --output <path>` 导出。

`request_completed` 在内联 `expect` 中默认只匹配动作开始后出现的请求,并支持 `method`、`status`、`status_class`、请求 body 和响应 body 子串约束。失败 envelope 会尽量包含 `error.diagnostics`。

`batch` 从 stdin 或 `--steps-file` 读取 JSON steps 数组或 `{steps, vars, finally}` 对象，作为一个浏览器插件 action 顺序执行。默认遇到首个失败步骤停止，单步可设置 `continue_on_error:true`；默认自动包裹输入拦截生命周期，传 `--no-lifecycle` 可禁用。v2 字段支持 `${...}` 插值、单步 `set`、`when`、`retry` 和顶层 `finally`。

```bash
printf '%s' '[{"action":"click","args":{"target_text":"Save","expect":{"condition":"text_equals","target":"#status","text":"Saved","timeout_ms":5000}}},{"action":"read_page","args":{"mode":"summary"}}]' | ace-browser-host.exe batch --json --session demo
```

所有响应都使用统一 envelope：

```json
{"ok":true,"data":{}}
{"ok":false,"error":{"code":"daemon_not_running","message":"ace-browser-host daemon is not running"}}
```

`status --json` 也使用 envelope。daemon 不可连接时返回 `ok:true` 且 `running:false`，方便 ACECode 展示健康状态，而不是把 daemon 未启动视为 CLI 执行失败。

连接 daemon 成功后，`status --json` 会包含 `queued_actions` 和 `pending_actions`。排查超时时，`queued_actions > 0` 通常表示 action 尚未被插件 poll 走，`pending_actions > 0` 通常表示 action 已经进入等待结果阶段。

ACECode 的内置 `browser_start` 会自动检查并后台启动 `ace-browser-host serve --json --port 52007`。手动运行 `serve` 主要用于独立调试 host、插件或 CLI 协议。

## 日志

host 不向 stdout 写日志，stdout 只保留 JSON envelope。host CLI、daemon 和插件上报的浏览器 action 摘要统一写入 ACECode 日志目录：

```text
%USERPROFILE%\.acecode\logs\ace-browser-host-YYYY-MM-DD.log
```

插件不会直接写本地文件；它会把 `action_start`、`action_finish`、`result_posted`、`screenshot_start`、`screenshot_finish` 等摘要 POST 到 host 的 `/plugin/log`，由 host 写入同一个日志文件。日志只记录 action id、session、action、耗时、错误码、截图大小等摘要，不记录截图 base64、表单填充值、输入文本或网络 body。

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
- `POST /plugin/log`
- `POST /shutdown`

`/plugin/hello` 由 `ace-browser-bridge` 上报 `protocol_version`、插件版本、浏览器类型和 capabilities。当前协议版本为 `0.1`；如果插件协议与 CLI daemon 不兼容，页面动作会返回：

```json
{"ok":false,"error":{"code":"version_mismatch","message":"..."}}
```

截图和 PDF 等二进制结果由插件返回 base64 给 CLI，CLI 写入本地文件后移除 `data` 字段，只把路径、MIME type 和大小返回给 ACECode。
