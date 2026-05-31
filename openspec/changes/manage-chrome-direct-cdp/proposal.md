## Why

`ace-browser-host` 目前把 ready 和 command path 绑定在 Chrome extension 的 `chrome.debugger` 通道上。目标页一旦被 DevTools、其他扩展、浏览器内部调试端或残留 attachment 占住,host 就会返回 `cdp_unavailable` / `Another debugger is already attached`,即使页面本身完全可被 CDP 控制。

对用户的主要场景来说,浏览器能力应该是 agent 的基础原子能力:主模型盲操作 bug 系统,低算力视觉模型只负责看截图证据。这里需要的是一个稳定的浏览器控制面,而不是反复杀进程或降级到鼠标模拟。

## What Changes

- `ace-browser-host` 增加 direct-CDP backend:由 host 启动或连接 Chrome,持有浏览器级 CDP WebSocket,并在 host 内管理 page target/session。
- `ensure-ready` 以 direct-CDP 可用作为首选 ready 条件,不再要求 extension 先连接。
- `status` 暴露 backend 状态:active backend、direct-CDP ready、Chrome 进程/CDP 连接、debug port/ws url、extension fallback 状态。
- CLI page actions 和 `cdp` 优先走 direct-CDP backend;extension backend 保留为兼容路径,仅在 direct backend 不可用或 action 未覆盖时使用。
- raw CDP 通过 host 持有的 CDP WebSocket 执行,避免 extension `chrome.debugger` 的 per-tab 独占限制。

## Capabilities

### New Capabilities

- `browser-direct-cdp`: host-managed Chrome/CDP lifecycle, page target sessions, raw CDP execution, and direct action routing.

### Modified Capabilities

- `ace-browser-bridge-readiness`: ready state no longer requires extension connectivity when direct-CDP is ready.
- `ace-browser-bridge-tools`: tool prompt and client cache should treat direct-CDP as the primary browser backend.

## Impact

- `ace-browser-host/src/main.cpp`: Chrome discovery/launch, DevToolsActivePort handling, CDP WebSocket client, daemon state/readiness, direct command routing.
- `src/tool/ace_browser_bridge/client.cpp`: status caching and readiness checks accept direct-CDP readiness.
- `src/tool/ace_browser_bridge/browser_tools.cpp`: update injected browser guidance from extension-first to host/direct-CDP-first.
- `tests/browser/ace_browser_host_cli_test.cpp`: focused tests for readiness/status/backend routing contracts.
- `docs/ace-browser-bridge.md` and `ace-browser-host/README.md`: document direct backend and extension fallback.

## Non-Goals

- 不移除现有 extension bridge。
- 不把公司 bug 系统流程固化进浏览器工具。
- 不把每个前端动作都改成 OS pointer/vision-first;DOM/CDP 仍是主路径。
- 不要求用户手动开启固定 9222 端口;host 默认自己管理 Chrome 调试入口。
