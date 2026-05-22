# ace-browser-bridge 手工 smoke

这份清单用于补充 `scripts/smoke.ps1` 的静态检查，覆盖必须在真实浏览器里确认的插件行为。

1. 构建或定位 `ace-browser-host.exe`。普通 ACECode 工具调用会自动后台启动 daemon；需要独立调试时可手动启动：

   ```powershell
   build/ace-browser-host/Debug/ace-browser-host.exe serve --json --port 52007
   ```

2. 在 `chrome://extensions` 启用 Developer mode，使用 Load unpacked 加载 `ace-browser-bridge` 目录。
3. 打开插件 popup，确认显示端口 `52007`、插件版本、连接状态和最近错误区域。
4. 在普通 HTTPS 页面打开 ACECode，启用 `ace_browser_bridge.enabled=true` 后调用 `browser_status`，确认 `running=true`、`extension_connected=true`、版本和 capabilities 出现。
5. 调用 `browser_open` 打开测试页面，确认浏览器创建类似 `ACE-a1b2c3` 的短 hash 标签组。
6. 用同一个 session 再次调用 `browser_open` 打开另一个 URL，确认复用原 tab；再传 `new_tab:true`，确认只在显式请求时新建 owned tab。
7. 调用 `browser_read_page`，确认返回 `snapshot_id`、页面文本和 `@e` refs。
8. 调用 `browser_click` 或 `browser_type`，确认页面出现蓝绿色 operation overlay，动作结束后 overlay 自动清理。
9. 调用 `browser_enable` 启用 `pointer`，再用 `browser_hover` 或 `browser_drag` 确认 CDP pointer summary 返回 mode、speed、path point count。
10. 调用 `browser_network start/list/detail`，确认标签组切换 network 状态色，`detail` 返回请求摘要。
11. 停止 daemon，确认插件 popup 变为断开状态；重新启动 daemon 后点击 reconnect，确认状态恢复。
12. 禁用或卸载插件，确认 daemon `browser_status` 变为 `extension_connected=false`，页面动作返回 `extension_not_connected`。
13. 对 owned/adopted tab 分别调用 `browser_close_session`，确认 owned tab 被关闭，adopted tab 保留但解除 session 绑定。
