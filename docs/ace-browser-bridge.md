# ace-browser-bridge 集成说明

`ace-browser-bridge` 是浏览器插件，`ace-browser-host` 是本地 C++ CLI/daemon。ACECode 内置工具只保留 `browser_start`：它负责启动/检查 host，并在当前会话里追加一段 user-role 使用说明。后续页面操作由模型通过 `ace-browser-host(.exe)` CLI 子命令完成。

默认端口为 `52007`，daemon 只监听 `127.0.0.1`。`ace-browser-host(.exe)` 默认从 `acecode` 可执行文件同目录解析，不需要在配置里写路径。旧版本的路径覆盖字段仍会被兼容读取，但新配置不会再保存这些字段。

## 启用配置

```json
{
  "ace_browser_bridge": {
    "enabled": true,
    "tool_mode": "progressive",
    "default_mode": "auto",
    "pointer_speed": "normal",
    "status_cache_ttl_ms": 2000,
    "tool_timeout_ms": 30000,
    "os_pointer_enabled": false,
    "tab_group_enabled": true,
    "operation_overlay_enabled": true,
    "operation_overlay_watchdog_ms": 10000
  }
}
```

`tool_mode` 仍被兼容读取，但当前内置工具注册策略不再按模式暴露成批 `browser_*` 工具；启用后只注册 `browser_start`。渐进式披露从“动态增加工具 schema”改为“调用 `browser_start` 后追加 user_prompt”，避免每轮 system prompt 或 tool schema 变动。

TUI 中可以用 `/browser` 做当前会话级开关：

- `/browser` 或 `/browser status`：显示当前会话是否已注册 `browser_start`。
- `/browser on`：只为当前 TUI 会话启用 ACE Browser Bridge，不写入全局配置。
- `/browser off`：只为当前 TUI 会话移除 ACE Browser Bridge，不写入全局配置。
- `/browser toggle`：切换当前 TUI 会话状态。

Web 设置页的“工具 -> ACE Browser Bridge”开关会写入全局 `ace_browser_bridge.enabled` 配置，并同步 daemon 当前共享工具集。新会话默认按该全局配置启动。

## 模型工作流

1. 调用 `browser_start`。它会触发 host 状态检查和必要的 host auto-start。
2. 读取 tool result 中的 `running`、`extension_connected`、版本和 capabilities。
3. 使用注入的 user_prompt 中的 `ace-browser-host(.exe)` CLI 示例执行页面动作。
4. 开始一组浏览器动作前先执行 `block-input`，让页面显示 AI 正在操作并拦截用户输入。
5. 先 `read-page` 获取页面文本和 `@e` 元素引用，再交互。
6. 交互优先使用 `@e` ref；CSS selector 作为 fallback。
7. 所有浏览器动作结束后，单独执行 `unblock-input` 退出 block；需要清理标签页时再使用 `close-session`。

如果模型不能识图，截图仍可保存为文件，但应优先依赖 `read-page`、`evaluate`、`network` 和导出文件 metadata；只有必要时才请用户人工查看保存的截图/PDF。

## CLI 子命令

所有命令都返回统一 envelope：

```json
{"ok":true,"data":{}}
{"ok":false,"error":{"code":"...","message":"..."}}
```

常用命令：

```bash
ace-browser-host.exe start --json
ace-browser-host.exe status --json
ace-browser-host.exe block-input --json --session acecode-demo --watchdog-ms 300000
ace-browser-host.exe open --json --session acecode-demo --url https://example.com --timeout-ms 15000
ace-browser-host.exe find-tab --json --session acecode-demo --active
ace-browser-host.exe navigate --json --session acecode-demo --operation reload --timeout-ms 15000
ace-browser-host.exe read-page --json --session acecode-demo --mode summary
ace-browser-host.exe wait --json --session acecode-demo --condition element_visible --target @e15 --timeout-ms 5000
ace-browser-host.exe click --json --session acecode-demo --target @e15
ace-browser-host.exe fill --json --session acecode-demo --target @e1 --value "text"
ace-browser-host.exe type --json --session acecode-demo --target @e2 --text "abc" --submit
ace-browser-host.exe hover --json --session acecode-demo --target @e3
ace-browser-host.exe drag --json --session acecode-demo --from @e4 --to @e5
ace-browser-host.exe scroll --json --session acecode-demo --delta-y 700
ace-browser-host.exe evaluate --json --session acecode-demo --code "(() => document.title)()"
ace-browser-host.exe network --json --session acecode-demo --cmd start
ace-browser-host.exe network --json --session acecode-demo --cmd list --filter api
ace-browser-host.exe screenshot --json --session acecode-demo --output ./page.png
ace-browser-host.exe save-pdf --json --session acecode-demo --file-name page.pdf
ace-browser-host.exe list-tabs --json --session acecode-demo
ace-browser-host.exe unblock-input --json --session acecode-demo
ace-browser-host.exe close-session --json --session acecode-demo
```

需要完全自定义时仍可用底层 `command --json`：

```bash
echo {"session":"acecode-demo","action":"snapshot","args":{}} | ace-browser-host.exe command --json
```

## 交互模式

- `auto`：默认模式，优先使用 CDP pointer，失败时按动作 fallback 到 DOM 路径。
- `dom`：直接在页面 DOM 中执行 click、fill、type 等动作，速度快，适合普通表单。
- `cdp`：通过 Chrome Debugger API 发送鼠标、滚轮、键盘和文本输入事件。
- `os`：OS 级鼠标键盘模式，默认关闭；只有 `os_pointer_enabled=true` 时才允许。

长文本替换优先使用 `fill`。需要观察逐字输入、提交 Enter 或按键行为时使用 `type`。

## Human Pointer 速度

`pointer_speed` 支持 `fast`、`normal`、`slow` 和 `custom`。`custom` 使用 `pointer_custom`：

```json
{
  "ace_browser_bridge": {
    "pointer_speed": "custom",
    "pointer_custom": {
      "move_duration_ms_min": 180,
      "move_duration_ms_max": 650,
      "click_hold_ms_min": 45,
      "click_hold_ms_max": 120,
      "typing_delay_ms_min": 20,
      "typing_delay_ms_max": 90,
      "jitter_px": 2.0,
      "max_path_points": 80
    }
  }
}
```

单次 pointer action 可传 `--speed`、`--duration-ms`、`--hold-ms` 和 `--jitter` 覆盖全局 profile，bridge 会把数值 clamp 到允许范围。

## AI 操作提示与输入拦截

`block-input` / `unblock-input` 用于把一整组浏览器操作包起来：

```bash
ace-browser-host.exe block-input --json --session acecode-demo --watchdog-ms 300000
ace-browser-host.exe open --json --session acecode-demo --url https://example.com
ace-browser-host.exe read-page --json --session acecode-demo --mode summary
ace-browser-host.exe unblock-input --json --session acecode-demo
```

`block-input` 会显示更明显的蓝绿色边框和提示条，并拦截用户鼠标、键盘、滚轮和触摸输入。如果当前 session 还没有绑定 tab，命令会返回 `pending:true`，后续 `open` 或 `find-tab` 成功后自动应用遮罩。`unblock-input` 应作为浏览器动作结束后的独立命令执行，避免异常路径留下遮罩。

## 版本与本地边界

CLI daemon 和插件握手时会交换 `protocol_version`。当前协议版本为 `0.1`；版本不兼容时页面动作返回 `version_mismatch`，`status --json` 会显示 `protocol_version`、`host_protocol_version` 和 `version_compatible`。

daemon 只监听 `127.0.0.1:52007`，CLI 和插件端点使用本地调用头区分调用方。ACECode 不直接访问 daemon HTTP 端点，只调用 `ace-browser-host`。

插件 trace 保存在内存环形缓冲区，默认不落盘。工具结果会截断大文本、网络 body 和 evaluate value；截图/PDF 的 base64 在 CLI 写入本地文件后会被移除。

## 手工验证清单

- `browser_start` 只注册一个内置工具，并在第一次调用时注入 CLI user_prompt。
- daemon 未启动时 `browser_start` 自动启动 host；daemon 已启动但插件未连接时 `extension_connected=false`。
- `ace-browser-host.exe open/read-page/fill/click/close-session` 完成表单填写流程。
- CSS selector 找不到元素时，重新 `read-page` 并用 `@e` ref 操作。
- `wait` 等待元素可点击、文本出现、network idle。
- `network start/list/detail` 捕获 GET 请求并查看详情。
- `screenshot` 保存图片文件且不返回 base64。
- `save-pdf` 保存 PDF 并返回规范化路径。
- `click` 的 CDP mode 返回 mode、target、path point count 和 duration。
- `fast`、`normal`、`slow` profile 的 duration/hold/path summary 有明显差异。
- `hover` 打开 hover 菜单，`drag` 移动 slider 或 sortable element，`type` 输入文本并发送 Enter。
- owned/adopted tab 行为：`close-session` 关闭 owned tab，不关闭 adopted tab。
- 默认标签组标题使用类似 `ACE-a1b2c3` 的短 hash；显式 `--group-title` 会覆盖默认标题。
- AI 操作期间页面出现蓝绿色渐变边框和 `AI 正在操作浏览器`，并拦截页面内容区输入。
