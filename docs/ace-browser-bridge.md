# ace-browser-bridge 集成说明

`ace-browser-bridge` 是浏览器插件，`ace-browser-host` 是本地 C++ CLI/daemon。ACECode 内置的 `browser_*` tools 只调用 `ace-browser-host` 的 JSON 契约，不直接访问浏览器插件内部协议。

默认端口为 `52007`，daemon 只监听 `127.0.0.1`。

## 启用配置

在 ACECode 配置中加入：

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

`ace-browser-host(.exe)` 默认从 `acecode` 可执行文件同目录解析，不需要在配置里写路径。旧版本的路径覆盖字段仍会被兼容读取，但新配置不会再保存这些字段。

`tool_mode`：

- `progressive`：默认只暴露核心工具，通过 `browser_enable` 按需启用更多组。
- `compact`：只暴露核心工具，不动态启用更多工具。
- `full`：启动时暴露全部浏览器工具。

## 推荐工作流

1. `browser_status`：确认 CLI daemon、浏览器插件和版本状态。
2. `browser_open` 或 `browser_find_tab`：打开新页面或复用用户当前页面。
3. `browser_read_page`：读取 snapshot、页面文本和 `@e` 元素引用。
4. `browser_wait`：导航或点击后等待 URL、文本、元素或网络条件。
5. `browser_enable`：按需启用 `interaction`、`pointer`、`capture`、`network`、`diagnostics`、`advanced`。
6. 交互时优先使用 `browser_read_page` 返回的 `@e` ref；CSS selector 作为 fallback。
7. 需要排查时用 `browser_trace` 和 `browser_list_tabs`。
8. 结束任务时用 `browser_close_session` 清理 owned tabs。

常规诊断路径使用 `browser_status`。TUI 中也可以用 `/browser` 做当前会话级开关：

- `/browser` 或 `/browser status`：显示当前会话是否已注册 `browser_*` tools。
- `/browser on`：只为当前 TUI 会话启用 ACE Browser Bridge tools，不写入全局配置。
- `/browser off`：只为当前 TUI 会话移除 `browser_*` tools，不写入全局配置。
- `/browser toggle`：切换当前 TUI 会话状态。

Web 设置页的“工具 -> ACE Browser Bridge”开关会写入全局 `ace_browser_bridge.enabled` 配置，并同步 daemon 当前共享工具集。新会话默认按该全局配置启动。

## 工具组

- 核心：`browser_status`、`browser_open`、`browser_find_tab`、`browser_navigate`、`browser_read_page`、`browser_wait`、`browser_enable`、`browser_close_session`
- `interaction`：`browser_click`、`browser_fill`、`browser_type`
- `pointer`：`browser_hover`、`browser_drag`、`browser_scroll`
- `capture`：`browser_screenshot`、`browser_save_pdf`
- `network`：`browser_network`
- `diagnostics`：`browser_trace`、`browser_list_tabs`
- `advanced`：`browser_evaluate`、`browser_upload`

## 交互模式

- `auto`：默认模式，优先使用 CDP pointer，失败时按动作 fallback 到 DOM 路径。
- `dom`：直接在页面 DOM 中执行 click、fill、type 等动作，速度快，适合普通表单。
- `cdp`：通过 Chrome Debugger API 发送鼠标、滚轮、键盘和文本输入事件。
- `os`：OS 级鼠标键盘模式，默认关闭；只有 `os_pointer_enabled=true` 时才允许。

长文本替换优先使用 `browser_fill`。需要观察逐字输入、提交 Enter 或按键行为时使用 `browser_type`。

## Human Pointer 速度

`pointer_speed` 支持：

- `fast`：更快的移动、点击和输入节奏。
- `normal`：默认平衡值。
- `slow`：适合观察动画、录屏和调试。
- `custom`：使用 `pointer_custom`。

示例：

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

单次 pointer action 可传 `speed`、`duration_ms`、`hold_ms` 和 `jitter` 覆盖全局 profile，bridge 会把数值 clamp 到允许范围。

调试 pointer path 时，可在单次 `browser_click`、`browser_hover` 或 `browser_drag` 中传 `debug_visualization=true`。插件只在当前动作里临时绘制路径，之后自动清理。

## 版本与本地边界

CLI daemon 和插件握手时会交换 `protocol_version`。当前协议版本为 `0.1`；版本不兼容时页面动作返回 `version_mismatch`，`browser_status` 会显示 `protocol_version`、`host_protocol_version` 和 `version_compatible`。

daemon 只监听 `127.0.0.1:52007`，CLI 和插件端点使用本地调用头区分调用方。ACECode 不直接访问 daemon HTTP 端点，只调用 `ace-browser-host`。

插件 trace 保存在内存环形缓冲区，默认不落盘。工具结果会截断大文本、网络 body 和 evaluate value；截图/PDF 的 base64 在 CLI 写入本地文件后会被移除。

## 已知限制

- CDP 需要浏览器插件声明 `debugger` permission；如果目标 tab 已被 DevTools 占用，CDP attach 可能失败并返回 `cdp_unavailable`。
- 跨域 iframe 内部元素不能直接从父页面操作；需要打开 iframe 的真实 URL 后再读页面和交互。
- `os` mode 会占用真实鼠标和窗口焦点，默认关闭。
- 某些页面对 synthetic DOM events 支持不完整；遇到 DOM click/fill 不生效时，改用 `cdp` 或 `auto`。
- 截图和 PDF 由插件返回二进制数据给 CLI，CLI 在本地写文件；tool result 只返回路径 metadata。

## 手工验证清单

- daemon 未启动、daemon 已启动但插件未连接、插件已连接三种 `browser_status`。
- 打开 `https://httpbin.org/forms/post`，读取 snapshot refs，填写字段，通过 `@e` 点击提交。
- CSS selector 找不到元素时，重新 `browser_read_page` 并用 `@e` ref 操作。
- `browser_wait` 等待元素可点击、文本出现、network idle。
- `browser_network start/list/detail` 捕获 GET 请求并查看详情。
- `browser_screenshot` 保存图片文件且不返回 base64。
- `browser_save_pdf` 保存 PDF 并返回规范化路径。
- `browser_click` 的 CDP mode 返回 mode、target、path point count 和 duration。
- `fast`、`normal`、`slow` profile 的 duration/hold/path summary 有明显差异。
- `browser_hover` 打开 hover 菜单。
- `browser_drag` 移动 slider 或 sortable element。
- `browser_type` 输入文本并发送 Enter。
- `browser_trace` 返回最近操作摘要。
- owned/adopted tab 行为：`browser_close_session` 关闭 owned tab，不关闭 adopted tab。
- 标签组在 operating、network、waiting、error 状态下切换颜色。
- AI 操作期间页面出现蓝绿色渐变边框和 `AI 正在操作浏览器`，并拦截页面内容区输入。
