# Design: add-ask-question-policy

## Context

`AskUserQuestion` 有两条实现路径（`src/tool/ask_user_question_tool.cpp`）：

- **TUI 路径** `create_ask_user_question_tool`：工具线程写 `TuiState` ask 字段 → PostEvent → 在 `state.ask_cv.wait(...)` 上无限期阻塞，直到 overlay 回答 / Esc / abort。
- **daemon async 路径** `create_ask_user_question_tool_async`：走 `ToolContext::ask_user_questions`（JSON in/out）→ `AskUserQuestionPrompter::prompt` 推 `question_request` 事件 + 50ms 轮询 condvar，直到 `question_answer` / abort。prompter 已有 `timeout_` 成员（默认 0=禁用，超时返回 `cancelled=true` + 发 `question_timeout` Error 事件），但 `session_registry.cpp:866` 实例化时从不传值。

两路都通过 `goal_unattended(ctx)` 识别 active goal。新契约不再直接自动应答，而是覆盖为 Timeout(30)，保留 UI 回答窗口。

`ToolContext` 已具备 `goal_unattended_active` 探针；`AgentLoop` 持 `loop_cfg_`（`set_agent_loop_config` 注入）并在工具调用前组装 tool_ctx —— 这是策略注入的天然位置。既有 `current_permission_mode` 继续供 Plan 工具使用，但 AskUserQuestion 策略不读取它。

配置遵循 sparse-on-write（默认值不落盘），`load_config` 有逐字段校验 + LOG_WARN 规范化先例（如 `max_iterations`、`alt_screen_mode`）。

## Goals / Non-Goals

**Goals:**

- YOLO 放开全部工具权限（包括工作区外首次写入），不弹权限确认，也不改变 `AskUserQuestion` 的业务澄清语义。
- active goal 下 `AskUserQuestion` 提供固定 30 秒回答窗口，超时自动采纳推荐项并继续。
- TUI / daemon 行为一致：同一策略解析函数、同一自动应答文案契约。
- goal 的工具权限自动放行行为保持不变。

**Non-Goals:**

- 不做 TUI overlay 倒计时动画 / 逐秒刷新 UI（v1 静态提示行即可）。
- 不改 Web 前端协议（复用既有 `question_closed` + reason 通路）。
- 不做 per-tool-call 粒度的策略覆盖（模型侧不可指定策略）。
- 不引入 `/question-policy` 斜杠命令（本期 CLI + config 足够；后续有需求再加）。

## Decisions

### D1. 配置放 `agent_loop` 段，两个字段

```json
"agent_loop": { "question_policy": "ask", "question_timeout_seconds": 60 }
```

- `question_policy`: `"ask"`（默认，现行为）/ `"deny"` / `"timeout"`。非法值 LOG_WARN 归一化为 `"ask"`。
- `question_timeout_seconds`: 仅 policy=timeout 时读取，默认 60，clamp 到 [5, 3600]（越界 LOG_WARN 归位）。
- `AppConfig` 增加运行时标记 `question_policy_explicit`（不序列化）：`load_config` 里 JSON 显式含 `question_policy` 键时置 true，用于日志/metadata 区分「默认 ask」与「用户写了 ask」。
- 备选：独立顶层 `question` 段 —— 拒绝，单一工具的交互策略归属 agent loop 交互语义，与 `max_iterations` 同段更聚拢。

### D2. 策略解析 = 纯函数层 `src/tool/question_policy.{hpp,cpp}`

```cpp
enum class QuestionPolicy { Ask, Deny, Timeout };
struct ResolvedQuestionPolicy {
    QuestionPolicy policy;
    int timeout_seconds;      // 仅 Timeout 有意义
    const char* origin;       // "explicit" | "default"（日志/metadata 用）
};
ResolvedQuestionPolicy resolve_question_policy(
    const std::string& configured_policy,  // 归一化后的配置值
    bool policy_explicit,                  // 配置或 CLI 显式指定过
    int configured_timeout_seconds);
```

优先级（高→低）：

1. **active goal 运行时覆盖** —— 工具入口将生效策略覆盖为 Timeout(30)。
2. **显式配置**（config 或 CLI）—— 按面值使用。
3. **默认** —— Ask。

YOLO 不参与上述解析；它只由 `PermissionManager` 决定工具权限是否自动放行。若命中显式 `Deny` 规则，AgentLoop 直接返回规则拒绝结果而不打开 permission prompt；其余 YOLO 工具调用全部自动放行。

进 `acecode_testable`，Node 依赖零，gtest 直接覆盖全部分支组合。

### D3. 策略经 `ToolContext` 注入，不让工具层碰 AppConfig

`ToolContext` 增加一个 `std::function<ResolvedQuestionPolicy()> question_policy` 探针。`AgentLoop` 组装 tool_ctx 时绑定 lambda，内部只解析显式配置与默认值。工具入口读到 `goal_unattended_active()` 时将策略覆盖为 Timeout(30)。空函数 = Ask（独立调用 ToolExecutor 的旧行为不变）。

- 备选：工具注册时闭包捕获 config —— 拒绝，探针式 late-binding 才能反映 active goal 和当前会话策略。

### D4. Deny 返回 success=true 的自动应答，不是失败

`make_policy_denied_ask_result()` 使用 success=true，避免模型把策略自动应答当成工具故障反复重问：

- `success = true`
- 文案：说明当前会话禁止交互提问、指示模型选推荐项或最合理假设、简述决策后继续推进。
- `metadata` 增加 `{"ask_user_question_auto": {"mode": "deny", "origin": "<origin>"}}`，TUI/Web 转录行可标注「已按策略自动应答」。

`待开发.md` 原文写的「可恢复错误」形态被否决：success=true 指令式应答不会让模型进入工具失败重试循环。

### D5. Timeout：自动采纳每个 question 的第一个选项

工具 description 已约定「推荐项排第一」，第一个选项即预选项。两路实现：

- **TUI**：`state.ask_cv.wait` 改 `wait_for` 循环（500ms 粒度）带 deadline；overlay 打开期间正常交互；到点后工具线程自己收 overlay（复用现有清理块）并合成 answers（每题第一选项 label）。overlay 顶部提示行显示「N 秒无操作将自动选择推荐项」（静态文案，不逐秒刷新）。
- **daemon**：`AskUserQuestionPrompter` 超时分支返回 `timed_out=true`。`prompt()` 支持 per-call timeout override；active goal 每次调用传 30 秒，普通会话则使用配置在会话创建时确定的等待窗口。JSON 桥响应透传 `"timed_out": true`，async 工具收到后合成 first-option answers。
- prompter 默认 timeout 仍在 `session_registry.cpp` 创建 entry 时注入；active goal 不依赖重建 prompter，而是每次 `prompt()` 传 30 秒 override。
- 用户超时后的答案不可再提交：`notify_response` 现有「未知 request_id = no-op」语义天然兜底。
- ToolResult `metadata.ask_user_question_auto = {"mode": "timeout", "seconds": N}`；`output` 前缀注明「用户 N 秒未回答，已自动采纳推荐项」，让模型知道这不是用户的真实意志。
- 备选：TUI 采纳当前焦点项（更贴 Claude Code「预选项」语义）—— 拒绝，多题流程下逐题焦点状态复杂、边际价值低；v1 统一第一选项，双端行为可预测。

### D6. CLI `--question-policy <ask|deny|timeout[:秒数]>`

- TUI：`src/cli/interactive_options.{hpp,cpp}` 解析（`timeout:120` 冒号语法），`src/main.cpp` 启动时覆写内存中 `cfg.agent_loop.question_policy` + 置 explicit 标记（不落盘），与 `--yolo` 模式一致。
- daemon：`src/daemon/cli.cpp` 同款参数，作用于 worker 的 cfg 副本，对该 daemon 全部会话生效。
- 非法值：启动报错退出（fail fast，与 `--resume` 未知 session 的处理风格一致）而非静默归一化 —— CLI 是显式意志，静默改写比报错更危险。

## Risks / Trade-offs

- **[timeout 自动选错项]** 推荐项不一定真是用户想要的 → 文案 + metadata 明确标注「自动采纳」，模型被告知可在后续被用户纠正；默认 policy 仍是 ask，timeout 是用户显式 opt-in。
- **[prompter 配置 timeout 会话中途不可变]** 普通配置秒数仍在会话创建时固化；goal 的 30 秒窗口由 per-call override 保证动态生效。
- **[goal 30 秒可能自动选错]** 用户未必在 30 秒内看到提问 → 结果文案和 metadata 明确标记「自动采纳」，保留后续纠正空间。
- **[explicit 标记与 sparse-on-write 冲突]** 用户手写 `"question_policy": "ask"` 后 save_config 会因等于默认值而丢键，下次加载 explicit 丢失；当前 ask 的面值行为不受影响。
- **[TUI wait_for 500ms 轮询]** 相比纯 condvar 多了空转 → 与 prompter 既有 50ms 轮询同风格，500ms 粒度对秒级 timeout 足够，CPU 开销可忽略。

## Migration Plan

非 goal 的默认 `question_policy="ask"` 行为与引入本特性前一致，YOLO 亦使用同一提问语义。active goal 的提问从「立即自动决策」迁移为「弹 UI 等待 30 秒，超时自动采纳推荐项」，无数据迁移。

## Open Questions

（无 —— YOLO 与提问策略已解耦，active goal 的 30 秒覆盖由 TUI deadline 和 daemon per-call timeout 两条路径共同实现。）
