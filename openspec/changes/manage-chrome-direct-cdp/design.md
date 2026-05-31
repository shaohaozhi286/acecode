## Overview

借鉴 `vercel-labs/agent-browser` 的关键边界:daemon 持有唯一的浏览器级 CDP WebSocket,页面操作通过 `Target.createTarget` / `Target.attachToTarget` 的 session id 进入具体 tab。DevTools 或外部调试入口如果需要存在,也应该被 host 代理,而不是让任意外部客户端直接抢占目标页。

现有 ACE 架构可分两层演进:

```
agent/browser tool
        |
        v
ace-browser-host daemon
        |
        +-- direct-CDP backend (primary)
        |       |
        |       +-- host-launched Chrome
        |       +-- browser WebSocket
        |       +-- target/session map
        |
        +-- extension backend (fallback)
                |
                +-- service worker poll/result
                +-- chrome.debugger
```

## Direct Chrome Lifecycle

默认路径:

1. host 确认 daemon 运行。
2. daemon 启动 Chrome,参数包含 `--remote-debugging-port=0` 和 host-managed 独立 `--user-data-dir`。
3. daemon 轮询 profile 下的 `DevToolsActivePort` 文件,拿到动态端口和 browser WebSocket path。
4. daemon 连接 `ws://127.0.0.1:<port><path>`,用 `Browser.getVersion` 验证连接。
5. daemon 通过 `Target.getTargets` 复用可控 page;没有 page 时 `Target.createTarget("about:blank")`。
6. daemon 对 page target 执行 `Target.attachToTarget({ flatten: true })`,记录 target id 和 session id。

后续可以支持两种可选模式:

- `ACE_BROWSER_CHROME` / `--chrome-path`:指定 Chrome 可执行文件。
- `ACE_BROWSER_USER_DATA_DIR`:指定 host-managed Chrome profile。默认使用 `.acecode/browser/chrome-profile` 持久 profile,让内部系统登录态可复用。
- `--cdp-url` / `ACE_BROWSER_CDP_URL`:连接外部 CDP provider,不接管进程生命周期。

## Backend Selection

ready 条件:

- direct-CDP backend ready: daemon running, browser WebSocket alive, at least one attached page session。
- extension backend ready:保留现有 extension fresh + protocol compatible。

`status.ready = direct_ready || extension_ready`。`status.backend` 表示当前首选 backend:

- `direct_cdp`:direct ready。
- `extension`:direct 不可用但 extension ready。
- `none`:都不可用。

command routing:

1. direct backend 覆盖的 actions 直接在 host 内执行。
2. direct 不支持的 actions,如果 extension fresh,走原 poll/result queue。
3. 两者都不能执行时返回 `backend_unavailable`,diagnostics 明确 direct/extension 的状态。

## Direct Action Scope

第一阶段覆盖 unblock 用户痛点所需的最小闭环:

- readiness/status。
- `cdp` raw method dispatch。
- `open` / `navigate` / `read_page` / `evaluate`。
- session/tab 基础映射。

第二阶段覆盖 richer atomic actions:

- `click` / `fill` / `type` / `wait` / `assert`。
- screenshot/pdf/upload/network/console/devtools proxy。

## Why This Fixes Debugger Ownership

extension backend 用的是 `chrome.debugger.attach`。Chrome 对同一 target 的 debugger attachment 有独占语义,所以任意 DevTools、其他 extension 或残留 attachment 都可能让它失败。

direct-CDP backend 不调用 `chrome.debugger`;它连接的是 browser-level DevTools WebSocket,再用 target sessions 操作页面。目标页 session 由 host 统一管理,外部调试入口应该通过 host proxy 申请临时 session,从而避免“多个工具各自抢 tab”的状态。

## Risks

- Chrome 路径和 profile 锁定在 Windows 上最容易出错。默认使用独立持久 profile；如果该 profile 已有 host-managed Chrome 运行,优先复用 `DevToolsActivePort` 连接。
- 自研 WebSocket/CDP client 必须处理 masking、ping/close、large frame 和 id/session 路由。第一阶段保持同步请求模型,避免过早引入复杂事件总线。
- direct backend 初期 action 覆盖有限。fallback 要保留,但错误不能再把 extension failure 误报为整体不可用。
