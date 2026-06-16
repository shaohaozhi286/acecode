## MODIFIED Requirements

### Requirement: MCP 服务器连接
系统 SHALL 在进程启动时为每个已配置的 MCP 服务器记录运行时条目，并异步为每个启用的服务器创建 `mcp::client` 基类的合适子类实例、完成 MCP 初始化握手、发现工具并注册到进程共享的 `ToolExecutor`。MCP 服务器初始化 SHALL NOT 阻塞 TUI 首屏渲染，也 SHALL NOT 阻塞 daemon HTTP/WebSocket 服务启动。

各 transport 对应的具体类型为：

- `transport: "stdio"` → `mcp::stdio_client`
- `transport: "sse"` → `mcp::sse_client`（旧版 `/sse` + `/message` 双端点）
- `transport: "http"` → `mcp::streamable_http_client`（2025-03-26 Streamable HTTP 单端点）

对于 sse/http 传输，系统还 SHALL 在 `initialize` 之前应用 `auth_token`、`headers` 与 `timeout_seconds` 等可选设置。

#### Scenario: stdio 成功连接
- **WHEN** 系统启动且 stdio 服务器 `command` 有效
- **THEN** 系统在后台构造 `mcp::stdio_client`、完成 `initialize()` 握手，并标记该服务器为 `Connected`
- **AND** 该服务器暴露的工具被注册到共享 `ToolExecutor`

#### Scenario: SSE 成功连接
- **WHEN** 系统启动且 sse 服务器暴露旧版 `/sse` + `/message` 端点并可达
- **THEN** 系统在后台构造 `mcp::sse_client`、应用 headers/token/timeout 后完成 `initialize()` 握手，并标记该服务器为 `Connected`
- **AND** 该服务器暴露的工具被注册到共享 `ToolExecutor`

#### Scenario: Streamable HTTP 成功连接
- **WHEN** 系统启动且 http 服务器暴露 Streamable HTTP 单端点（默认 `/mcp`）并可达
- **THEN** 系统在后台构造 `mcp::streamable_http_client`、应用 headers/token/timeout 后完成 `initialize()` 握手，保存服务器返回的 `Mcp-Session-Id`，并标记该服务器为 `Connected`
- **AND** 该服务器暴露的工具被注册到共享 `ToolExecutor`

#### Scenario: 传输专属字段相互独立
- **WHEN** stdio 条目里包含 `url` 字段或 http 条目里包含 `command` 字段
- **THEN** 系统只读取与当前 `transport` 相关的字段，其他字段被忽略

#### Scenario: HTTP 连接失败
- **WHEN** sse/http 服务器的 `url` 不可达、TLS 校验失败或初始化握手返回失败
- **THEN** 系统记录包含失败原因的错误日志，标记该服务器为 `Failed`，不影响其他服务器和 acecode 启动

#### Scenario: stdio 连接失败
- **WHEN** stdio 服务器的 `command` 不存在或子进程启动失败
- **THEN** 系统记录错误日志，标记该服务器为 `Failed`，不影响其他服务器和 acecode 启动

#### Scenario: MCP 初始化不阻塞可用启动
- **WHEN** 一个已配置 MCP 服务器启动缓慢或在握手阶段等待超时
- **THEN** TUI 仍 SHALL 完成首屏渲染并允许用户输入
- **AND** daemon 仍 SHALL 完成 HTTP/WebSocket 服务启动
- **AND** 该服务器的最终状态 SHALL 在后台更新为 `Connected`、`Failed`、`TimedOut` 或 `Cancelled`

## ADDED Requirements

### Requirement: MCP 启动状态可见
系统 SHALL 为每个 MCP 服务器维护明确的运行时状态，至少包含 `Starting`、`Connected`、`Failed`、`Disabled`、`Cancelled` 与 `TimedOut`。用户可见的 MCP 状态展示 SHALL 使用该状态，而不是仅通过工具数量推断服务器是否可用。

#### Scenario: /mcp 展示启动中服务器
- **WHEN** 用户在某个 MCP 服务器仍在后台初始化时执行 `/mcp`
- **THEN** 输出中该服务器 SHALL 显示为 `starting`
- **AND** 输出 SHALL 保留 transport 类型和定位字符串

#### Scenario: /mcp 展示失败原因
- **WHEN** 某个 MCP 服务器初始化失败
- **THEN** `/mcp` 输出 SHALL 显示该服务器为 `failed`
- **AND** `/mcp list` SHALL NOT 列出该服务器的工具

#### Scenario: 后台启动完成提示
- **WHEN** MCP 服务器从 `Starting` 转为 `Connected`、`Failed`、`Cancelled` 或 `TimedOut`
- **THEN** 当前运行表面 SHALL 收到状态更新并以适合该表面的方式展示或记录

### Requirement: TUI 非阻塞 MCP 启动
TUI SHALL NOT wait for MCP server initialization before entering its interactive render loop. If one or more MCP servers are configured, TUI SHALL show a startup status or system message indicating MCP startup is in progress, then update the visible state when startup settles.

#### Scenario: 慢 MCP 不阻塞首屏
- **WHEN** 用户启动 TUI 且某个 MCP stdio 服务器在 `initialize()` 中耗时较长
- **THEN** TUI SHALL render the main chat interface before that server finishes initialization
- **AND** user input SHALL be accepted while MCP startup continues

#### Scenario: 无 MCP 配置保持安静
- **WHEN** `mcp_servers` 为空
- **THEN** TUI SHALL NOT show MCP startup progress
- **AND** startup behavior SHALL remain equivalent to the previous no-MCP path

#### Scenario: MCP 启动失败不覆盖其他状态
- **WHEN** MCP startup reports a failure while another TUI status such as model activity is active
- **THEN** TUI SHALL add a warning or system message without corrupting the active model status display

#### Scenario: TUI 右侧栏展示 MCP 启动和工具
- **WHEN** TUI regular sidebar is visible and one or more MCP servers are configured
- **THEN** the sidebar SHALL include an MCP section showing each configured server's runtime state
- **AND** servers in `Starting` state SHALL show an animated loading indicator
- **AND** connected servers SHALL show their registered MCP tool count without expanding the full tool list

#### Scenario: TUI 无右侧栏时展示 MCP loading
- **WHEN** TUI regular sidebar is not visible and one or more MCP servers are still starting
- **THEN** the conversation area SHALL show an animated MCP loading indicator
- **AND** the indicator SHALL stop once no MCP server remains in `Starting`

### Requirement: 首次模型请求协调 MCP 启动
When a TUI session has configured MCP servers that are still starting, the first user submission SHALL coordinate with MCP startup before the provider request is built. The coordination SHALL be bounded and cancellable so MCP cannot indefinitely block the first model request.

#### Scenario: 首次提交等待 MCP 快速完成
- **WHEN** the user submits the first prompt while MCP startup is still in progress
- **AND** all pending MCP servers settle within the configured or default first-turn wait budget
- **THEN** the first provider request SHALL include every MCP tool registered by the settled servers

#### Scenario: 首次提交等待超时后继续
- **WHEN** the user submits the first prompt while MCP startup is still in progress
- **AND** MCP startup does not settle within the first-turn wait budget
- **THEN** the prompt SHALL continue with the currently registered tools
- **AND** TUI SHALL show a warning that some MCP tools may be unavailable for that turn

#### Scenario: 后续轮次包含后到工具
- **WHEN** an MCP server connects after the first provider request was sent
- **THEN** subsequent provider requests SHALL include that server's registered tools

### Requirement: MCP 工具注册并发安全
MCP tool registration and unregistration SHALL be safe while agent turns, slash commands, and MCP startup workers are active. A tool list snapshot for a provider request SHALL remain stable for that request even if MCP tools are registered or removed by another thread.

#### Scenario: 工具注册不影响正在构建的请求
- **WHEN** an MCP server finishes startup while an agent turn is building its provider request
- **THEN** that request SHALL use one consistent tool definition snapshot
- **AND** the newly registered MCP tools SHALL be available to later requests

#### Scenario: 禁用服务器不破坏正在执行的工具
- **WHEN** `/mcp disable <name>` unregisters a server's tools while a previously copied tool invocation is already running
- **THEN** the in-flight invocation SHALL finish or fail through its existing client path without accessing invalid executor entries

#### Scenario: 过期启动结果不覆盖新状态
- **WHEN** a server is reconnected or disabled while an earlier background startup attempt is still finishing
- **THEN** the stale startup result SHALL NOT re-register tools or overwrite the newer server state

### Requirement: MCP runtime 可被 daemon 和 desktop 复用
MCP runtime lifecycle SHALL be implemented in a surface-neutral component that can be owned by both the TUI process and the daemon process. Desktop SHALL obtain MCP capability through the daemon-owned runtime rather than launching separate desktop-owned MCP clients.

#### Scenario: TUI owns process-local MCP runtime
- **WHEN** ACECode runs in TUI mode
- **THEN** the TUI process SHALL own one MCP runtime associated with its shared `ToolExecutor`
- **AND** MCP tools registered by that runtime SHALL be visible to the TUI agent loop

#### Scenario: Daemon owns process-local MCP runtime
- **WHEN** ACECode runs in daemon mode and `mcp_servers` is configured
- **THEN** the daemon process SHALL be able to own one MCP runtime associated with the daemon's shared `ToolExecutor`
- **AND** daemon-created sessions SHALL see MCP tools through the same executor snapshot mechanism used for built-in tools

#### Scenario: Desktop delegates MCP to daemon
- **WHEN** ACECode Desktop starts or reuses its supervised daemon
- **THEN** Desktop SHALL NOT create a separate MCP runtime in the desktop shell process
- **AND** desktop-backed sessions SHALL use the daemon-owned MCP runtime for MCP tool availability

### Requirement: MCP shutdown includes pending startup
MCP shutdown SHALL cancel pending startup work before releasing connected clients and clearing discovered tools. Shutdown SHALL be safe to call multiple times.

#### Scenario: Exit while MCP still starting
- **WHEN** the user exits while one or more MCP servers are still in `Starting`
- **THEN** shutdown SHALL mark pending startup work cancelled or stopping
- **AND** shutdown SHALL prevent later background completions from registering tools after the manager has begun teardown

#### Scenario: Connected clients still cleaned up
- **WHEN** shutdown runs after some servers have connected and others are still starting
- **THEN** connected clients SHALL be released according to the existing MCP cleanup rules
- **AND** unconnected startup attempts SHALL NOT block process exit indefinitely
