# ask-question-policy Spec

## ADDED Requirements

### Requirement: question_policy 配置字段与校验
系统 SHALL 在 `config.agent_loop` 下支持 `question_policy`（`"ask"` / `"deny"` / `"timeout"`，默认 `"ask"`）与 `question_timeout_seconds`（默认 60，合法区间 [5, 3600]）。非法 `question_policy` 值 MUST 归一化为 `"ask"` 并 LOG_WARN；越界 `question_timeout_seconds` MUST 归位默认值并 LOG_WARN。默认值 MUST 不落盘（sparse-on-write）。`load_config` MUST 在 JSON 显式包含 `question_policy` 键时记录 explicit 标记（运行时字段，不序列化）。

#### Scenario: 默认配置不落盘
- **WHEN** 用户从未配置 question_policy 且 ACECode 保存配置
- **THEN** 序列化产物不含 `question_policy` / `question_timeout_seconds` 键

#### Scenario: 非法策略值归一化
- **WHEN** 配置文件含 `"question_policy": "banana"`
- **THEN** 加载后策略为 `"ask"`，explicit 标记为 false，且输出 LOG_WARN

#### Scenario: 超时秒数越界归位
- **WHEN** 配置 `"question_timeout_seconds": 2`
- **THEN** 加载后值为 60 且输出 LOG_WARN

#### Scenario: 显式配置置 explicit 标记
- **WHEN** 配置文件含 `"question_policy": "ask"`
- **THEN** 加载后策略为 ask 且 explicit 标记为 true

### Requirement: 策略解析优先级
系统 SHALL 通过纯函数 `resolve_question_policy` 解析生效策略：显式配置（config 或 CLI）按面值生效，否则为 Ask。permission mode MUST NOT 改变提问策略；YOLO 只影响工具权限确认。active goal MUST 在工具入口将策略覆盖为 Timeout(30)。

#### Scenario: YOLO 保持默认提问
- **WHEN** permission_mode 为 `"yolo"` 且用户未显式配置 question_policy
- **THEN** 解析结果为 Ask，AskUserQuestion 正常打开提问 UI

#### Scenario: YOLO 不压制显式 ask
- **WHEN** permission_mode 为 `"yolo"` 且用户显式配置 `question_policy="ask"`
- **THEN** 解析结果为 Ask，且打开提问 UI

#### Scenario: YOLO 不弹任何工具权限确认
- **WHEN** permission_mode 为 `"yolo"` 且模型调用任意需权限的工具，包括首次写入工作区外路径
- **THEN** 工具权限全部自动放行，不打开 permission prompt；AskUserQuestion 仍由 question_policy 独立决定是否打开提问 UI

#### Scenario: YOLO 命中显式拒绝规则时静默阻止
- **WHEN** permission_mode 为 `"yolo"` 且工具调用命中用户配置的 `Deny` 权限规则
- **THEN** 工具不执行并返回规则拒绝结果，但不打开 permission prompt

#### Scenario: goal 固定 30 秒提问窗口
- **WHEN** 会话处于 goal 无人值守模式且配置 `question_policy="timeout"`
- **THEN** AskUserQuestion 正常打开 UI 并使用 30 秒 timeout，不使用普通配置的秒数

#### Scenario: 默认路径维持现状
- **WHEN** 未配置策略且 permission_mode 为 `"default"`
- **THEN** 解析结果为 Ask，TUI/daemon 行为与引入本特性前一致

### Requirement: Deny 策略自动应答
生效策略为 Deny 时，AskUserQuestion MUST 不占用任何 UI 通道（TUI overlay / daemon question_request 事件均不触发），并立即返回 `success=true` 的 ToolResult：文案指示模型自行选择推荐项或最合理假设、简述决策并继续；metadata MUST 携带 `ask_user_question_auto` 对象标注 `mode="deny"` 与 origin。

#### Scenario: deny 下无 UI 交互
- **WHEN** 策略为 Deny 且模型调用 AskUserQuestion
- **THEN** TUI 不弹 overlay、daemon 不发 question_request，工具立即返回 success=true 自动应答

#### Scenario: deny 结果可被转录标注
- **WHEN** Deny 自动应答返回
- **THEN** ToolResult.metadata 含 `{"ask_user_question_auto": {"mode": "deny", ...}}`

### Requirement: Timeout 策略自动采纳
生效策略为 Timeout 时，AskUserQuestion MUST 正常发起 UI 交互；用户在 N 秒内回答则与 Ask 行为一致；到期无回答则 MUST 自动采纳每个 question 的第一个选项（label）作为答案并返回 `success=true`，output 前缀注明用户未在 N 秒内回答、已自动采纳推荐项，metadata 标注 `mode="timeout"` 与秒数。daemon 路径 MUST 关闭 pending question（`question_closed` 事件 reason=`"timeout"`），且超时后到达的用户回答 MUST 被忽略（no-op）。

#### Scenario: 超时前回答走正常路径
- **WHEN** 策略为 Timeout(60) 且用户在第 10 秒提交回答
- **THEN** 返回结果与 Ask 策略下的正常回答完全一致，无 auto 标注

#### Scenario: 超时自动采纳第一选项
- **WHEN** 策略为 Timeout(N) 且 N 秒内无人回答
- **THEN** 每个 question 以其第一个选项 label 作为答案返回 success=true，output 注明自动采纳

#### Scenario: daemon 超时清理前端 pending
- **WHEN** daemon 会话中 Timeout 到期
- **THEN** 发出 reason 为 `"timeout"` 的 `question_closed` 事件，此后该 request_id 的 `question_answer` 被忽略

#### Scenario: TUI 超时收起 overlay
- **WHEN** TUI 会话中 Timeout 到期且 ask overlay 仍打开
- **THEN** overlay 被关闭、临时导航状态被清理，工具返回自动采纳结果

### Requirement: CLI 覆盖参数
TUI 与 daemon 可执行入口 SHALL 支持 `--question-policy <ask|deny|timeout[:秒数]>`：解析成功则覆写本次进程内存中的 `agent_loop.question_policy`（及可选秒数）并置 explicit 标记，MUST 不写配置文件；非法取值 MUST 启动报错退出。

#### Scenario: CLI timeout 冒号语法
- **WHEN** 以 `--question-policy timeout:120` 启动
- **THEN** 会话内生效策略为 Timeout(120)，配置文件内容不变

#### Scenario: YOLO 保留 CLI 显式 ask
- **WHEN** 以 `--yolo --question-policy ask` 启动
- **THEN** AskUserQuestion 正常弹出交互，工具权限确认仍全部自动放行

#### Scenario: 非法 CLI 值直接失败
- **WHEN** 以 `--question-policy sometimes` 启动
- **THEN** 进程输出错误并以非零码退出

### Requirement: 双端一致性
TUI 路径与 daemon async 路径 MUST 使用同一策略解析函数与同一自动应答文案构造函数；策略经 `ToolContext` 探针注入（空探针 = Ask）。goal 的 30 秒覆盖 MUST 在两条路径上一致生效。

#### Scenario: 运行中切 YOLO 不关闭提问
- **WHEN** 会话中途切换到 YOLO 权限模式后模型调用 AskUserQuestion
- **THEN** 该次调用仍按 question_policy 打开提问 UI，但任何工具权限确认（包括工作区外首次写入确认）都不弹出

#### Scenario: goal daemon 调用级 timeout
- **WHEN** daemon 会话在创建后进入 active goal，模型调用 AskUserQuestion
- **THEN** prompter 为该次调用使用 30 秒 timeout override，超时发送 `question_closed(reason="timeout")` 并自动采纳推荐项

#### Scenario: 独立 ToolExecutor 调用不受影响
- **WHEN** 工具在无策略探针注入的上下文中执行（如单测直接调 ToolExecutor）
- **THEN** 行为等同 Ask 策略
