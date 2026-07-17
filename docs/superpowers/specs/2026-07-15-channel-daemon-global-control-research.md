# Research Plan: daemon 级 Channel 指令与全局控制面

> Core question: 在不把外部 IM 业务写入 ACECode 核心的前提下，能否把现有单会话 `/rc` 绑定升级为 daemon 级 channel 控制面，并安全复用 Hermes 风格的会话、模型、状态和中断指令？
> min_rounds: 2

## Dimensions

1. **Hermes gateway 指令语义** — 核对真实源码中的命令注册、会话作用域、并发控制、权限与返回形态，区分“看起来有命令”和“适合远程 channel 使用”。
2. **ACECode 可复用能力** — 盘点 daemon/session registry/command registry 已有的会话、模型、停止、状态、新建与恢复能力，以及当前 `/rc` 单会话绑定的硬约束。
3. **全局 channel 路由模型** — 明确未绑定工作区时如何选择活动会话、如何新建/切换会话、普通文本投递到哪里、多个 channel 用户或多个客户端如何隔离。
4. **安全与生命周期** — 分析 daemon 启动自动连接、权限白名单、危险指令确认、忙碌会话中断、重启恢复、重复投递与并发竞态。
5. **交付边界与演进顺序** — 将候选指令分为可直接实现、需要通用能力后实现、不建议实现，并给出 MVP 与后续阶段。

## Completion criteria

- [ ] Hermes 的候选命令均有源码级语义与作用域证据。
- [ ] ACECode 对每条候选命令都有现有能力或明确缺口的映射。
- [ ] 给出 daemon 级 channel 的状态机和消息路由规则，不依赖某个 IM 产品名称。
- [ ] 给出权限、确认、并发和重启恢复的失败模式。
- [ ] 形成按优先级排序的指令矩阵和推荐 MVP。
- [ ] 至少两轮探索，并由独立 verifier 给出 PASS。

## Scope

- **In**: `/new`、会话列表/选择、`/model`、`/status`、`/stop`、`/compact`、`/help` 等适合纯文本 channel 的通用控制指令；daemon 自动连接；全局会话路由；权限与确认。
- **Out**: 本轮不写产品代码；不改变外部 IM 登录/传输协议；不把供应商名称或业务逻辑写入 ACECode；不复刻依赖 TUI 控件的交互界面。

---

## Round 1 working findings

### 1. 当前状态

现有 `SessionChannelBinder` 是严格的一对一绑定器：配置中只保存一个
`remote_control.bound_session_id`，入站回调直接捕获该 session id，出站只订阅该
会话，重绑时通过 generation 丢弃旧会话事件。daemon 启动也只有在这个会话可恢复时
才会激活 channel。因此它不能自然承担“daemon 先连接、之后任意选择会话”的职责。

ACECode 已有可复用底层能力：

- `SessionClient::create_session/resume_session/list_sessions/send_input/abort`；
- `SessionRegistry::switch_model` 和按会话的 model/permission/busy 状态；
- daemon-native 的 `/compact`、`/goal`、`/plan`；
- 通用 channel-plugin host、token 校验、入站/出站有界队列。

缺失的是 daemon 级 channel 控制服务：连接生命周期、当前会话指针、命令解析与权限、
跨工作区会话聚合、以及命令结果的纯文本投影。

### 2. Hermes 可借鉴的不是命令表，而是 gateway 模式

Hermes 的中央命令注册表记录 alias、参数、`cli_only/gateway_only`；gateway 再按来源
构建 `session_key`，先拦截控制命令，普通文本才进入 agent。它还把 busy 期间可立即
执行的 `/stop`、`/approve`、`/deny` 与需要拒绝/排队的命令分开，并用统一 slash
allowlist 区分 admin 与普通用户。

但 Hermes 也存在不能照抄的表面能力：`/sessions` 在注册表中存在而 gateway 没有
canonical handler；`/queue` 主要只在 busy fast-path 接线；`/new` 的空闲/忙碌确认
语义不一致；`/help` 也不总按当前用户权限过滤。因此 ACECode 的帮助必须来自
“有 handler + 当前 surface 支持 + 当前角色允许”的交集。

### 3. 初步指令矩阵

| 分期 | 指令 | 结论 |
|---|---|---|
| MVP | `/help` | 新建 channel 专用帮助，不实例化 TUI `CommandContext`。 |
| MVP | `/status` | 展示 channel、当前会话、workspace、model、busy、queue；不得泄露 RC token。 |
| MVP | `/new [--workspace ref\|--no-workspace]` | 基于 daemon `create_session`；不能复用 TUI“清空当前会话”的 `/new`；标题留给后续 `/title` 通用接口。 |
| MVP | `/sessions [page]` | 聚合 active、注册 workspace 的持久会话和 no-workspace 会话。 |
| MVP | `/resume <id\|序号\|标题>`（可加 `/use` 别名） | 恢复并选择会话，只切路由，不重启 channel。 |
| MVP | `/model [name]`、`/models` | 只允许 session 级切换；远程不提供 `--global`。 |
| MVP | `/stop` | 只中止当前选中会话，不是停止整个 daemon。 |
| MVP | `/queue <text>` | busy 时显式排队；空闲时等价于普通输入。 |
| MVP | `/approve <request-id>`、`/deny <request-id>` | 否则远程执行遇权限请求会永久等待；必须绑定 session/request。 |
| MVP | `/whoami` | 显示 channel route/principal/允许命令，便于安全诊断。 |
| Phase 2 | `/compact`、`/goal`、`/plan`、`/title` | 有底层能力，但需要输出投影与 busy 语义。 |
| Phase 2 | `/<skill-name>`、`/undo` | 前者需共享 workspace-aware expansion；后者需破坏性确认。 |
| 暂不做 | `/memory` 写操作、`/mcp` 变更、`/tools`/`/skills` 管理、`/model --global` | 属全局或破坏性配置，远程风险高；TUI 交互依赖也强。 |

### 4. 架构方向

不应把 binder 扩成“绑定所有会话”，而应拆成：

```text
ChannelSupervisor   daemon 启动即激活/保活插件，与 session 无关
        │
        ▼
ChannelRouter       route_key -> selected_session_id
        │
        ├── slash ──> ChannelCommandService ──> 直接文本回复
        │
        └── text  ──> SessionClient::send_input(selected_session_id)
                              │
                              ▼
                      SessionEventBridge ──> channel 出站
```

“可操作所有工作区”定义为可列出、恢复和选择不同工作区的会话；普通文本在任一时刻
仍只能进入一个明确选中的会话，绝不能广播。channel 配置只保存 enable/auto-connect/
权限策略；动态的 route -> selected session 建议写独立原子状态文件，不再频繁整份改写
`config.json`。

### 5. Round 1 缺口结论

第一轮已证明底层能力足够，但仍缺四个产品级定义：连接与会话选择是否解耦、首次消息
落点、busy 时的切换策略、远程权限/追问闭环。Round 2 以这四项为边界继续核对，未再
扩大到实现细节。

---

## Round 2 final design

### 0. 2026-07-15 最新基线复核

本设计已从 `d9ae670` rebase 到当前稳定 `master@8b92eb9`。最新两笔稳定变更只影响
MCP stdio 启动与工具只读元数据，不推翻 gateway 架构：

- Windows MCP 现在用结构化 `command + argv + env` 启动原生程序，含空格和 shell
  元字符的安装路径已有测试；现有 setup 注入的 `klpa-light --mcp --cwd <root>` 形状
  无需修改；POSIX 仍走 joined command line，不能泛称跨平台 argv 已统一；
- `tools/list.annotations.readOnlyHint=true` 已映射为 ACECode 只读工具，自动通过普通工具
  权限并可进入只读并行组；false/缺失仍按可写工具处理；
- channel plugin host 不经过 `McpManager`，所以 daemon 自动连接、binding identity、route
  和 busy 规则不受这两笔提交影响；
- 开始实现前必须同步 `external/cpp-mcp@3cd98f1`，否则 Windows 结构化 argv 构造器不可用。

同时，KLPA `master@264589a` 与 setup `master@09f947a` 已真正完成 SQLCipher 七天消息
落库、2000 条缓存、`query_messages` MCP 和安装注入，不再只是计划。它对本设计的影响是：

- 自动创建的 no-workspace session 同样能使用 `query_messages`；
- 该工具声明 `readOnlyHint=true`，正常不会产生 `/approve` pending；
- 无需新增 `/messages` 指令，历史查询继续由 AI 通过通用 MCP 工具调用；
- 历史消息属于不受信任数据，不得把查询结果解释成 channel 控制指令。

当前主检出 `82404ec` 是从 `5db9a11` 分叉的 detached WIP，并非稳定 master。它包含有价值
但尚未合入的显式 no-workspace/global-skill 三态解析，也回退了 `d9ae670` 已有的原子入站
路由、立即确认和关闭排空保障。实现时不得以该 detached 检出为基线；若要复用 skill
解析，只能单独移植小范围 resolver，不能合并整条分叉。

### 1. 可行性结论

**可以实现。** 但正确形态不是“一个 channel 绑定全部 session”，也不是让 daemon
直接复用 TUI `CommandRegistry`。应新增 daemon 级 gateway 控制平面，并复用 TUI
命令背后的 session/model/goal/compact 等服务。

TUI 命令注册器依赖 `TuiState`、前景 `AgentLoop`、picker/editor 等交互对象，直接搬到
daemon 会把 TUI 生命周期带入通用服务。Hermes 也没有把一条消息广播给所有 session；
它维护“消息来源 -> 当前会话”的映射，再用命令改变这个指针。ACECode 应采用同一核心
思想，但把 workspace 与 session catalog 做完整。

### 2. 目标架构

```text
ChannelSupervisor
  daemon 启动时异步激活/保活插件，与 session 无关
        │
        ▼
ChannelRouter
  route_key -> selected_session_id
        │
        ├── slash ──> ChannelCommandService ──> 直接 channel 回复
        │
        └── text  ──> SessionClient::send_input(selected_session_id)
                              │
                              ▼
SessionEventBridge ──> 只把该 route 当前选中 session 的事件送回 channel
        │
        └── SessionCatalog ──> active + registered workspace disk + no-workspace
```

关键约束：

- channel 可以处于 `Connected + Unselected`，不再依赖 `/rc` 才启动；
- 每个 route 任一时刻只能选中一个 session，普通消息绝不广播；
- 入站先去重并区分 slash/普通文本；command 结果走直接回复，不进入模型 transcript，
  也不发送“思考中...”确认；只有成功选定并接受的普通模型输入才发送该确认；
- 旧 session 切走后可以继续后台执行，但不会再向该 route 发输出；MVP 在 busy 时拒绝
  切换，先避免这种隐式后台执行；
- 同一个 session 若被多个 route 主动选择，出站可以显式 fan-out；
- 选择与订阅都带 generation，换绑后的旧事件必须丢弃。

### 3. daemon 启动和首次消息

推荐状态机：

```text
Disabled
  -> StartingListener
  -> ActivatingPlugin
  -> Connected
  -> DegradedRetryWait -> ActivatingPlugin
```

- `enabled && auto_connect` 时 daemon 启动后异步连接，失败不阻塞 Web/daemon 启动；
- 插件未登录或临时退出时有限退避重试，route 选择不丢失；
- 重启时从独立状态文件恢复 `route_key -> selected_session_id`，session 延迟恢复；
- 没有有效选择时，第一条普通文本自动创建 no-workspace session，使用默认模型，然后
  投递该文本；因此用户正常聊天不需要先输入任何指令；
- 若用户要进入某个代码工作区，使用 `/workspaces`、`/sessions`、`/new --workspace`
  或 `/use` 显式选择；
- 已保存 session 缺失或 workspace 不可用时 route 进入 `Stale`，不执行旧 cwd，提示用户
  重新选择。下一条普通文本可按配置自动退回 no-workspace。

### 4. “全部工作区”的准确范围

当前代码可可靠聚合：

1. daemon 内存中活跃的 session；
2. `WorkspaceRegistry` 中已登记且可见的工作区磁盘 session；
3. daemon 兼容 cwd 的 session；
4. no-workspace session。

因此 UI/帮助文案应写“所有已知工作区”，不能宣称扫描任意未注册磁盘目录。若以后要
覆盖任意历史目录，需另建 workspace index/注册能力。

### 5. 推荐指令面

#### MVP：无需 Web/TUI 即可完成日常远程工作

| 指令 | 语义 | busy 时 | 权限/安全 |
|---|---|---|---|
| `/help` | 仅列出有 handler 且当前身份允许的指令 | 立即 | 所有人可用 |
| `/status` | channel、route、当前 session/workspace/model/busy/queue | 立即 | 已授权身份；不显示 token、绝对敏感路径 |
| `/whoami` | route/principal/角色/允许指令 | 立即 | 所有人可用 |
| `/workspaces [page]` | 列已知工作区 | 立即 | owner/admin，只读 |
| `/sessions [page] [--workspace ref]` | 分页列 session，默认隐藏 subagent/归档项 | 立即 | owner/admin，只读 |
| `/new [--workspace ref\|--no-workspace]` | 创建并选中新 session | 当前 session busy 时拒绝，要求先 `/stop` | owner/admin；不删除旧 session，无需二次确认 |
| `/use <id-prefix\|title>` | 恢复并选择已有 session；`/resume` 为别名 | busy 时拒绝 | owner/admin；不使用无 generation 的长期数字序号 |
| `/models`、`/model` | 列配置模型；按稳定 saved-model `name` 调 `SessionRegistry::switch_model` | 可允许，但明确“当前轮不变、下一轮生效” | owner/admin；使用脱敏 `ChannelModelSummary`，禁止 `--global`/CRUD/API key/base URL/headers |
| `/stop` | 中止当前选中 session 的当前轮 | 立即 | owner/admin；只作用当前 session，不停止 daemon |
| `/queue <text>` | 显式向当前 session FIFO 排入一条完整用户轮 | 任意 | owner/admin；回显队列深度 |
| `/approve <request-id>` | 批准当前 session 的指定可写工具权限请求 | 立即 | owner/admin；必须带 request id，禁止 daemon 全局“批准最老请求”；只读 MCP 不产生该请求 |
| `/deny <request-id>` | 拒绝指定权限请求 | 立即 | owner/admin；同上 |
| `/answer <request-id> <answer>` | 回答指定 AskUserQuestion | 立即 | owner/admin；绑定 session/request；结构化多问题可用 JSON 或分步语法 |
| `/pending` | 重列当前 route/session 未决权限请求与问题 | 立即 | owner/admin；用于断线恢复 |

普通文本：有 selected session 就进入该 session；无选择时自动建 no-workspace session。
未知 `/xxx` 不应直接投给模型，应该返回“未知指令，可用 /help 查看”。如果未来支持
skill slash，必须由明确 resolver 判定后再展开。

#### Phase 2：底层存在，但需要更完整的文本投影

- `/compact [focus]`：进入同一 session 队列，回显已排队和最终结果；
- `/goal ...`、`/plan ...`：复用 daemon builtin，但要把当前仅在 system/goal event 中的
  结果转换为 channel 文本；
- `/title <text>`：需要先新增 daemon 通用 `set_session_title` 接口；当前 `SessionOptions`
  和 `SessionClient` 不支持标题修改；
- `/<skill-name> ...`：复用 workspace-aware skill expansion，而不是把未知 slash 交给模型；
- `/undo [N]`：需要 `/confirm <operation-id>`，并限定当前 session。

#### 暂不做

- `/model --global`；
- 远程启停/重配 MCP；
- 安装、删除、启停 skill；
- `/memory` 编辑/删除；
- `/tools` 动态修改工具集；
- 任何依赖本机 picker/editor 的 TUI 命令；
- yolo/全局自动审批。

这些操作是进程级或破坏性配置，错误影响所有 session，不适合第一版纯文本 IM 控制。

### 6. busy、确认与远程交互规则

Hermes 的 busy fast-path 和普通 handler 存在语义不一致，ACECode 不应照抄。统一规则：

- 始终立即：`/help`、`/status`、`/whoami`、`/workspaces`、`/sessions`、`/models`、
  `/pending`、`/stop`、`/approve`、`/deny`、`/answer`；
- 始终可排队：普通文本、`/queue`；
- busy 时拒绝：`/new`、`/use`、`/resume`、`/undo`、`/compact`（第一版）；
- `/model` 可执行，但只影响下一轮并明确回执；
- 破坏性操作使用 `/confirm <operation-id>`，操作 id 带 TTL 且绑定 route/principal；
- 工具权限审批只用 `/approve|deny <request-id>`，不能与破坏性操作确认复用；
- QuestionRequest/PermissionRequest 必须主动发到 channel，否则 agent 会永久等待；
- `/pending` 属于 MVP，不能只依赖实时订阅：需给 permission/question prompter 增加统一 pending
  snapshot/query（permission 已有部分 snapshot 基础，question 仍需补齐），插件或 daemon 断线后
  可以重列 request id，再用 `/approve`、`/deny`、`/answer` 完成闭环。
- MCP `readOnlyHint` 只影响工具执行许可和并行度，不是 channel principal ACL。该元数据由
  本地已安装 MCP server 自报，因此“谁能安装/配置 MCP”仍属于本机管理员信任边界；不能
  因工具自报只读就提前开放远程 MCP 重配。

### 7. 通用协议演进

不需要立即升级为 v2，可在 v1 上增加可选字段。激活身份先与 session 解耦：

```json
{
  "type": "channel.activate",
  "binding_scope": "gateway",
  "binding_id": "stable-daemon-channel-id",
  "session_id": "",
  "capabilities": ["route-envelope-v1"]
}
```

- gateway 激活、健康检查、重复激活和 deactivate 均以稳定 `binding_id` 为身份，不能依赖
  “旧插件碰巧接受空 session_id”；
- `binding_scope` 缺失时继续以真实 `session_id` 走 legacy 行为；
- 新旧插件滚动升级测试必须覆盖：legacy session 激活不变、gateway 重复激活幂等、按
  `binding_id` 解绑不会影响 route state；

入站 envelope：

```json
{
  "text": "...",
  "channel_message_id": "...",
  "route": {
    "key": "opaque-default",
    "principal_id": "opaque-owner",
    "conversation_id": "opaque-chat"
  }
}
```

- 旧请求缺少 route 时，仅在兼容开关启用时映射为 `default`；
- plugin 负责把外部身份规范化成稳定、不含 PII 的 opaque route key；ACECode 不理解 IM
  业务字段；
- 出站补 `route_key` 和 `in_reply_to`，为多聊天来源、未来替身模式保留路由；
- 用 `(route_key, channel_message_id)` 做 TTL/LRU 去重，避免 ACECode 已接收但 HTTP 响应丢失
  后插件重试造成重复执行；
- 第一版授权模型明确为**单 owner/admin principal**。token 认证插件，`principal_id` 再认证
  远端操作者；两层不能混为一谈；
- `/workspaces`、`/sessions`、`/new --workspace`、`/use`、`/model`、`/stop`、
  `/approve`、`/deny`、`/answer`、`/pending` 全部只允许 owner/admin；
- 当前 self-only plugin 可始终发 `route.key=default, principal_id=owner`。兼容旧请求时只把
  default route 视为 owner，且需显式 `legacy_default_owner=true`；
- 未来多 principal 必须先引入 session 可见集、审批权和 fan-out ACL，不能让任意获准 route
  自动枚举所有工作区。

### 8. 配置、动态状态与兼容

用户配置仍在 `config.json`：

```json
{
  "remote_control": {
    "enabled": true,
    "auto_connect": true,
    "mode": "gateway",
    "default_channel": "...",
    "first_message": "auto_no_workspace",
    "legacy_default_route": true,
    "legacy_default_owner": true,
    "owner_principals": ["owner"],
    "max_routes": 8
  }
}
```

频繁变化的选择状态写独立、原子替换的 `~/.acecode/channel_state.json`：

```json
{
  "schema_version": 1,
  "routes": {
    "opaque-default": {
      "selected_session_id": "...",
      "workspace_hash": "...",
      "cwd_hint": "...",
      "no_workspace": true,
      "updated_at_ms": 0
    }
  }
}
```

- 不保存原始聊天/JID/principal；
- `cwd_hint` 只作恢复提示，必须用 session/workspace 元数据校验，绝不盲信外部路径；
- `mode` 缺失时继续走当前 legacy session binder，行为不变；
- 显式 `mode=gateway` 才启动新 Supervisor/Router，两个模式不能同时运行；
- 首次 gateway 启动可用合法旧 `bound_session_id` 初始化 default route，旧字段保留以便回滚；
- 新 ACECode 可先上线，旧 plugin 缺 route 时落 default；非 default 多 route 真正上线前再要求新 plugin。

### 9. 失败模式与验证门槛

- plugin 未安装/未登录：daemon 正常启动，channel 异步退避恢复；
- daemon 重启：连接自动恢复、route 选择恢复、session 延迟恢复；
- workspace 删除/离线：route 进入 Stale，禁止自动执行旧 cwd；
- duplicate inbound：只确认/执行一次；
- switch/destroy race：旧 generation 的订阅输出不能泄漏到新 session；
- plugin HTTP 已接收但响应丢失：去重仍成立；
- slash 分类：`/help`、`/status`、未知指令均不得先发“思考中...”；普通文本仅在 session
  接受后发送一次确认；
- 多 route 滥用：principal policy、route 上限、TTL 淘汰；
- daemon 关闭：停止接收新入站，有限排空直接回复，再停 Supervisor；
- legacy config：不启用 gateway 时全部现有 `/rc` 测试保持原样；
- 实现基线必须继续通过 `d9ae670` 的立即确认、原子 rebind/off 和有界 drain 回归测试；
- MCP 测试需同步新子模块，覆盖 Windows 含空格/元字符 argv，以及 readOnlyHint
  true/false/missing；确认只读 `query_messages` 不产生 pending，可写 MCP 仍产生 request id；
- 若 gateway 后续随 KLPA 原生包发布到 Linux/macOS，再补 POSIX plugin 生命周期、重启恢复和
  joined command-line MCP 测试；当前 Windows 增量包不因此扩大首期交付范围；
- 边界守卫：ACECode 源码和测试继续禁止供应商业务字符串。

### 10. 推荐实施顺序

0. 基于 `master@8b92eb9`（或更新稳定 master）实施并执行
   `git submodule update --init --recursive`；不得从 detached `82404ec` 分叉；
1. OpenSpec：定义 gateway/legacy 双模式、通用 route envelope、安全语义；
2. gateway `binding_id/binding_scope`、route envelope、去重和 legacy 兼容测试；
3. 从 Binder 抽出 session 无关的 `ChannelSupervisor`，实现 daemon 自动连接；
4. 原子 `ChannelRouteStore` 与一次性 legacy seed；
5. 跨已知 workspace/no-workspace 的 `SessionCatalog`；
6. `ChannelRouter + SessionEventBridge`，完成首消息自动 no-workspace、延迟恢复、generation；
7. 指令框架与第一批 `/help /status /workspaces /sessions /new /use /models /model /stop`，
   入站必须先 slash 分类，再决定是否发送“思考中...”确认；
8. pending snapshot/query、权限/追问出站与 `/approve /deny /answer /pending`；
9. `/queue /compact /goal /plan` 及统一文本投影；
10. setup 只注入通用配置；外网 mock 做冷启动、自动连接、首消息、重启恢复、工作区切换、
    plugin 故障恢复和 legacy 不回归的全链路验证。

### 11. 最终判断

这不是“小改 `/rc`”，而是一项独立的 daemon gateway 能力，适合保留在单独分支分阶段做。
它能达到用户设想的体验：开启 channel 后 daemon 自动连接；平时直接发消息即可；需要时
用纯文本指令创建/选择会话、切模型、停止任务和处理权限，不再依赖某个工作区先手工输入
`/rc`。同时仍严格遵守 ACECode 只提供通用能力、具体 IM 业务留在外部插件的边界。

---

## Completion check

- [x] Hermes 候选命令有源码级语义与作用域证据。
- [x] ACECode 候选命令映射到现有能力或明确缺口。
- [x] daemon 级连接、route、session 和事件状态机已定义。
- [x] 权限、确认、busy、重启、去重、兼容失败模式已定义。
- [x] 指令按 MVP、Phase 2、暂不做分层。
- [x] 完成两轮探索；最终独立 verifier PASS。
