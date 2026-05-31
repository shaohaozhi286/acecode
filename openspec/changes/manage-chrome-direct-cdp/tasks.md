## 1. OpenSpec

- [x] 1.1 编写 direct-CDP primary backend 的 proposal/design/spec/tasks
- [x] 1.2 `openspec validate manage-chrome-direct-cdp --strict` 通过

## 2. Direct-CDP Backend Foundation

- [x] 2.1 在 `ace-browser-host` 增加 Chrome executable discovery、launch args、持久 profile 和 `DevToolsActivePort` 读取
- [x] 2.2 增加最小 CDP WebSocket client,支持 `Browser.getVersion`、`Target.*`、`Runtime.evaluate`、`Page.navigate`
- [x] 2.3 在 daemon state 中记录 direct backend 状态、Chrome pid、debug port、ws url、target/session map

## 3. Readiness And Status

- [x] 3.1 修改 `status.ready` / `ensure-ready` 为 direct-CDP 优先,extension 为 fallback
- [x] 3.2 `status` 返回 backend diagnostics,能区分 direct/CDP/extension 的失败原因
- [x] 3.3 客户端 status cache 接受 direct-CDP ready

## 4. Command Routing

- [x] 4.1 `cdp` raw command 走 host-managed direct CDP
- [x] 4.2 `open` / `navigate` / `evaluate` / `read_page` 第一阶段走 direct CDP
- [x] 4.3 未覆盖动作保留 extension fallback,并返回明确 backend diagnostics

## 5. Prompt / Docs / Tests

- [x] 5.1 更新 `browser_start` 注入提示和 docs,说明 direct-CDP 是首选 backend
- [x] 5.2 增加 readiness/status/router 单测
- [x] 5.3 运行 OpenSpec、CMake/unit tests、focused host smoke
