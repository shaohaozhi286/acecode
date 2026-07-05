# 子代理(Subagent)体系

实现参考文档 — 覆盖 `spawn_subagent` / `wait_subagent` 工具、Web「后台任务」面板与 TUI 支持。
对应提交:`dd10b15`(工具内核,daemon)→ `2c20b5b`(Web 后台任务面板 + parent 持久化)→ `35b986f`(TUI 支持)。

## 1. 设计概览

参照 Claude Code 的 Task 工具语义,落在 ACECode 自己的多会话基建上:

- **子代理 = SessionRegistry 里的一个普通会话**:独立 SessionManager / AgentLoop /
  PermissionManager / ProviderSlot,上下文与父会话完全隔离。没有为子代理发明新的执行容器。
- **父上下文只吃回最终答复**:`wait=true` 时父会话阻塞至子会话本轮结束,把最后一条非空
  assistant 消息带回;子会话的中间过程(工具调用、流式输出)一个 token 都不进父上下文。
- **持久化归属**:子会话 meta 记录 `parent_session_id`(空省略,老 meta 向后兼容)。
  这让「子会话不进常规会话列表、只出现在父会话的后台任务面板」在 daemon 重启后依然成立,
  也让深度限制在 resume 后继续生效。
- **深度限制**:子代理不能再派生(`SessionEntry::subagent_depth`,resume 时从
  `meta.parent_session_id` 非空恢复为 1)。防递归失控。
- **UI 归属主会话**:子会话不出现在侧栏 / 全局搜索;Web 在主会话标题栏挂「后台任务」
  面板,TUI 在右侧 sidebar 显示运行中任务。权限确认 / AskUserQuestion 冒泡到主会话 UI
  回答,答案按 `session_id` 路由回子会话。

## 2. 工具语义

### spawn_subagent

| 参数 | 说明 |
|---|---|
| `prompt`(必填) | 子会话的首条用户消息。以 `/` 开头时按子会话 cwd 走 `try_expand_skill_command`(与 Web 输入框同一套 skill 命令展开),原文存 `display_text` |
| `wait`(默认 true) | true:阻塞至子会话本轮结束,返回最终答复;false:点火即返,返回 `session_id`(流水线接力 / fan-out) |
| `model` | 可选 saved_models 名称;缺省用 daemon/TUI 默认模型 |
| `timeout_seconds` | 可选等待上限;0/缺省 = 无限(父会话 abort 仍可打断) |

- `is_read_only = true`:spawn 本身自动放行不弹确认;子会话内部的危险操作由子会话自己的
  PermissionManager 把关(权限模式继承父会话)。
- 子会话继承父会话的 cwd 与权限模式。父会话不在 registry(TUI 主会话)时,权限模式经
  `SubagentToolDeps::fallback_permissions` 继承。
- 成功 spawn 后调用 `SubagentToolDeps::on_spawn(child_id, prompt)`(TUI 用它登记任务并
  订阅子会话事件;daemon 留空)。
- 工具结果 `metadata.subagent_session_id` 随 `tool_end` 事件透传到前端(Web 靠它即时发现新任务)。

### wait_subagent

`wait_subagent(session_id, timeout_seconds?)`:等待某个已点火的子会话空闲并取回其**最新**
答复(baseline=0,不要求本次调用后新产生)。配合 `spawn(wait=false)` 做并行 fan-out 再逐个 join。

### 等待判定(wait_for_subagent,250ms 轮询)

- `send_input` 是异步入队,只看 `!is_busy()` 会在"还没开始"时误判 → 观察到过 busy
  (observed_busy)后回落才算完成。
- 兜底:2 秒内从未观测到 busy(极快 turn 在轮询间隙完成)时以"出现新 assistant 消息"为
  完成信号;持续无新消息 60 秒 → Timeout(不销毁子会话,可稍后 `wait_subagent` 收)。
- 父会话 abort(Esc / 停止按钮)→ 传播 `loop->abort()` 给子会话,但**不销毁**——已产出
  的工作保留,可在面板 / `/tasks` 里查看。

## 3. 后端实现地图

| 文件 | 职责 |
|---|---|
| `src/tool/spawn_subagent_tool.{hpp,cpp}` | 两个工具本体 + `SubagentToolDeps{registry, client, config, fallback_permissions, on_spawn}`。daemon 里 deps 用 shared_ptr 延迟回填(ToolExecutor 先于 SessionRegistry 构造,见 worker.cpp) |
| `src/session/session_storage.{hpp,cpp}` | `SessionMeta.parent_session_id`(空省略序列化);`purge_session_files(project_dir, id)` 删 jsonl + meta + `<id>/` 目录(web purge 路由与 TUI `/tasks clear` 共用) |
| `src/session/session_manager.{hpp,cpp}` | `set_parent_session_id` / `current_parent_session_id`;start_session 重置、ensure_created/update_meta 落盘、resume_session 读回 |
| `src/session/session_registry.{hpp,cpp}` | `SessionEntry::{subagent_depth, parent_session_id}`;make_entry_locked 从 opts 写入 / 从 resumed_meta 恢复(恢复时强制 depth≥1);list_active 透出 parent 字段 |
| `src/session/session_client.hpp` | `SessionOptions::{subagent_depth, parent_session_id}`、`SessionInfo::parent_session_id` |
| `src/web/server_helpers.cpp` | `sessions_for_workspace(..., parent_filter)`:空 = 常规列表**排除**全部子会话;非空 = 只返回该父会话的子任务(active 部分跳过 workspace 过滤);`session_info_to_json` / `session_meta_to_json` 输出 `parent_session_id` |
| `src/web/routes/routes_sessions.cpp` | `GET /api/sessions?parent=<id>`;`DELETE /api/sessions/:id?purge=1`(仅子会话,主会话 400,busy 409;destroy 后 purge_session_files) |
| `src/web/routes/routes_workspaces.cpp` | workspace 路由的 `?parent=` 同语义 |
| `src/daemon/worker.cpp` | daemon 注册点(registry/client 就绪后回填 deps) |

### HTTP / 协议增量(详见 docs/daemon-api.md)

- `GET /api/sessions` 与 `GET /api/workspaces/:hash/sessions` 默认排除子会话;
  `?parent=<session_id>` 反查该父会话的子任务(后台任务面板数据源)。
- `SessionSummary` 增 `parent_session_id` 字段(普通会话为空串)。
- `DELETE /api/sessions/:id?purge=1` = 面板「清除」:销毁 + 永久删磁盘;
  `400 only subagent sessions can be purged` / `409 session is busy`。
- `tool_end` payload 的 `metadata` 原样透传(含 `subagent_session_id`),这是前端即时发现
  新子任务的通道之一。

## 4. Web「后台任务」面板

### 数据流

```
GET /api/sessions?parent=<主会话id>          ← 任务快照(标题/tokens/时间戳/busy)
父会话 tool_end(spawn_subagent/wait_subagent) → refetch
session_status 帧(未知 busy 会话)            → refetch   ← wait=true 期间父 turn 无
                                                            tool_end,靠它感知新子任务
子会话 WS 事件(busy/usage/tool_start/tool_end/session_updated)→ 卡片增量
```

- `web/src/lib/subagentTasks.js` — 纯状态层(Node 单测):快照归一化、refetch 合并
  (保留实时聚合字段 toolCount/lastTool 与本地 aborted 标记)、事件增量、分组/徽标计数、
  格式化(`34.0k tokens · 2 tools · bash`、耗时)。
- `web/src/lib/useSubagentTasks.js` — hook:REST + WS 增量 + **对运行中任务
  `connection.retainSession`(不依赖面板开关)** —— 这是子会话的 permission_request /
  question_request 能到达 App 全局监听并冒泡的前提。parent 切换有迟到响应守卫。
- `web/src/components/SubagentPanel.jsx` — overlay(absolute 挂在 ChatView 消息区容器内,
  只覆盖聊天区,不动 Sidebar/SidePanel):运行中 / 已完成分组卡片;运行中卡片右上停止按钮;
  「清除」批量 purge 已结束任务;「查看会话」原地切到只读 transcript(复用主会话
  Message/ToolBlock 紧凑渲染,`useSessionTranscript` live:'auto' 实时跟尾,AskUserQuestion
  工具行过滤不显示)。
- ChatView 标题栏最右「后台任务」按钮:有任务或面板开着才出现,徽标 = 运行中数。

### 权限 / 提问冒泡

- App.jsx 的全局 connection 监听本来就把**任何被订阅会话**的 `permission_request` 弹成全局
  PermissionModal(payload 带 session_id,`sendDecision` 自动路由)——所以权限冒泡的成本
  只是保证子会话被订阅(useSubagentTasks 做了)+ 来源标签。
- question 的可见性筛选(App 的 `visibleQuestionReq` 与 ChatView 的 `questionForView`)
  放宽为「activeId 或 activeId 的子任务」;ChatView 经 `onSubagentTasksChange` 上报
  子任务 id→标题映射给 App。
- PermissionModal / QuestionPicker 增可选 `originLabel`(「来自后台任务:〈标题〉」)。

## 5. TUI 支持

### SubagentHost(src/tui/subagent_host.{hpp,cpp},进 acecode_testable)

SessionRegistry / LocalSessionClient 无 web 依赖 → TUI 进程直接实例化。host 职责:

- `on_spawned`(工具 `on_spawn` 回调):登记运行中任务 + `client_.subscribe` 订阅子会话
  事件。**订阅竞态兜底**:on_spawn 在 send_input 之后调用,极快 turn 可能在订阅前发完
  BusyChanged(false),订阅后补查一次「已空闲且已有 assistant 答复」即移除。
- 事件桥接(回调在子会话 loop 线程):`BusyChanged(false)` → 移除任务(右侧列**只显示
  运行中**,用户决策);`SessionUpdated` → 标题更新;`PermissionRequest` →
  `Deps::on_permission_request` 冒泡。快照经 `Deps::publish_tasks` 交付
  (main.cpp 写入 `TuiState.subagent_tasks` + PostEvent)。
- `/tasks` 后端:`list_tasks`(运行中 registry + 磁盘 parent 匹配的已结束)、
  `abort_task`、`clear_settled`(purge_session_files)、`respond_permission`。
- 不显式退订:dispatcher 生命周期 = SessionEntry;host 析构(main 栈)→ registry 析构
  → 逐个 abort+join 子会话。

### TUI 主会话不在 registry 里的两个后果

1. 权限模式继承走 `SubagentToolDeps::fallback_permissions`(main.cpp 注入进程级
   PermissionManager)。
2. 深度限制天然成立:主会话按 depth 0 处理;子会话在 registry 里,再 spawn 被拒。

### Overlay 占用协议(并发确认的串行化)

子代理并发后,confirm/ask overlay 有三类占用者:主会话工具确认(`on_tool_confirm`)、
子会话的 AskUserQuestion(见下)、远程权限泵。协议:

- 占用条件 `!confirm_pending && !ask_pending`;等待在 `TuiState::overlay_cv` 上
  (100ms 超时轮询,各自的 abort 信号可打断)。
- 释放点(confirm submit / close_ask_overlay / shutdown 清理)`overlay_cv.notify_all()`。
- 远程权限泵不阻塞(UI 线程):子会话的 permission_request 入
  `TuiState::remote_confirm_queue`(host 回调 + PostEvent),CatchEvent 入口处在 overlay
  空闲时弹队占用 confirm overlay,`confirm_remote_session_id/request_id` 标记远程;
  submit 分支据此改走 `SubagentHost::respond_permission`(Allow→allow /
  AlwaysAllow→allow_session / Deny→deny),不 notify confirm_cv。
- confirm overlay 渲染 `confirm_origin_label`(`[subagent] 〈标题〉`)。

### AskUserQuestion 不需要桥接

TUI 与子会话**共享 ToolExecutor**,子会话执行的本来就是 TUI 版 ask 工具(直连
TuiState overlay,工具线程 wait ask_cv 天然带回结果)。只需两点:入口在 overlay_cv 上
排队等空闲;从 `ctx.session_manager->current_parent_session_id()` 判定子会话来源,设置
`ask_origin_label`(prompt 行显示)。daemon 路径不受影响(worker 注册的是
`create_ask_user_question_tool_async`,走 AskUserQuestionPrompter 事件)。

### 右侧栏与 /tasks

- `render_regular_sidebar` 的「Background Tasks」区块:bash 前台任务(原有)+ 子代理
  运行中任务(`●` + 标题截断 + 耗时);anim_thread 在 `subagent_tasks` 非空时持续
  tick(否则 `wait=false` 点火后主会话 idle,耗时不刷新)。
- `/tasks [list|abort <id>|clear]`(builtin_commands.cpp,`CommandContext::subagent_host`
  仅斜杠 dispatch 路径注入):abort 支持 id 前缀唯一匹配;clear 与 Web「清除」同语义。

## 6. 测试地图

| 文件 | 覆盖 |
|---|---|
| `tests/tool/spawn_subagent_tool_test.cpp` | deps 缺失 / 空 prompt / fire-and-forget / 深度拒绝 / wait 全链路 / parent 持久化(ChildRecordsParentSessionId)/ resume 恢复身份(ResumeRestoresSubagentIdentityFromMeta)/ wait_subagent |
| `tests/session/session_storage_test.cpp` | meta parent_session_id 回环 + 空省略 |
| `tests/web/web_server_smoke_test.cpp` | 列表隐藏 + ?parent= 反查;purge 护栏(主会话 400)+ 真删 + 普通 DELETE 不删盘 |
| `tests/tui/subagent_host_test.cpp` | 快照发布/BusyChanged 移除;list 合并 + clear 只删已结束;abort 路由 |
| `web/src/lib/subagentTasks.test.js` | 归一化/合并/事件增量/aborted 保持/分组/格式化(13 例) |

统一模式:EchoStreamProvider stub 让子会话 turn 真实完成(消息落盘、busy 迁移),不打真实 LLM。

## 7. 已知限制与 future work

- **no_workspace 父会话**:子会话继承父 cwd 但按普通 workspace 会话建档;`?parent=` 的
  磁盘扫描按请求 workspace 的 project_dir,所以 no_workspace 父的子任务 active 时可见、
  daemon 重启后扫不到(v1 接受)。
- **运行中卡片的工具计数**(Web)从订阅时刻累积:页面中途打开时已完成任务只显示
  tokens/耗时,undercount 是有意取舍。
- **远程权限请求无取消回收**(TUI):子会话被 abort 后已入队/已展示的请求悬空,用户回应
  是 no-op(unknown request_id),无害但 overlay 需手动关。
- **purge 不清理 attention 记录**(session_read_state):量小,残留无 UI 影响。
- **TUI 右侧无已结束痕迹**(用户决策"只显示运行中");已结束任务经 `/tasks` 或 Web 面板查看。
- Future:父会话工具行的子代理流式进度(tool_update 通道,跨端受益);
  `.acecode/agents/*.md` 自定义 agent 类型(专属 system prompt / 工具白名单);
  Web 面板运行中 transcript 的输入接管。

## 8. 典型用法:多阶段流水线

每个阶段一个 SKILL.md(职责 + 读上一阶段交接 md + 写本阶段交接 md),出口写
「完成后调用 `spawn_subagent(prompt="/next-stage-skill <args>", wait=false)` 点火下一阶段」。
用户只需在第一个会话输入 `/stage-1 <需求>`,阶段间上下文零互通、信息只走交接文档,
每阶段在后台任务面板 / `/tasks` 独立可查。
