# Design: add-ask-question-policy

## Context

`AskUserQuestion` 有两条实现路径（`src/tool/ask_user_question_tool.cpp`）：

- **TUI 路径** `create_ask_user_question_tool`：工具线程写 `TuiState` ask 字段 → PostEvent → 在 `state.ask_cv.wait(...)` 上无限期阻塞，直到 overlay 回答 / Esc / abort。
- **daemon async 路径** `create_ask_user_question_tool_async`：走 `ToolContext::ask_user_questions`（JSON in/out）→ `AskUserQuestionPrompter::prompt` 推 `question_request` 事件 + 50ms 轮询 condvar，直到 `question_answer` / abort。prompter 已有 `timeout_` 成员（默认 0=禁用，超时返回 `cancelled=true` + 发 `question_timeout` Error 事件），但 `session_registry.cpp:866` 实例化时从不传值。

两路都已有 goal 无人值守豁免：`goal_unattended(ctx)` 为 true 时直接返回 `make_goal_unattended_ask_result()`（success=true 自动应答，绝不占 UI）。

`ToolContext` 已具备 `current_permission_mode`（YOLO 时返回 `"yolo"`，含 `is_dangerous()`）与 `goal_unattended_active` 两个探针；`AgentLoop` 持 `loop_cfg_`（`set_agent_loop_config` 注入）并在 `agent_loop.cpp:1770` 附近组装 tool_ctx —— 这是策略注入的天然位置。

配置遵循 sparse-on-write（默认值不落盘），`load_config` 有逐字段校验 + LOG_WARN 规范化先例（如 `max_iterations`、`alt_screen_mode`）。

## Goals / Non-Goals

**Goals:**

- 无人值守（YOLO / headless / 定时任务 / 子代理）下 `AskUserQuestion` 不再无限期阻塞。
- 策略语义与「权限全放开」解耦：独立 `question_policy` 配置，YOLO 只做隐式默认映射，显式配置永远赢。
- TUI / daemon 行为一致：同一策略解析函数、同一自动应答文案契约。
- goal 无人值守现有行为零变化（仍是最高优先级豁免，绕过一切策略配置）。

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
- `AppConfig` 增加运行时标记 `question_policy_explicit`（不序列化）：`load_config` 里 JSON 显式含 `question_policy` 键时置 true。这是「显式配置胜过 YOLO 隐式映射」的判据 —— sparse-on-write 下无法从值本身区分「默认 ask」与「用户写了 ask」。
- 备选：独立顶层 `question` 段 —— 拒绝，单一工具的交互策略归属 agent loop 交互语义，与 `max_iterations` 同段更聚拢。

### D2. 策略解析 = 纯函数层 `src/tool/question_policy.{hpp,cpp}`

```cpp
enum class QuestionPolicy { Ask, Deny, Timeout };
struct ResolvedQuestionPolicy {
    QuestionPolicy policy;
    int timeout_seconds;      // 仅 Timeout 有意义
    const char* origin;       // "config" | "cli" | "yolo-implicit" | "default"（日志/metadata 用）
};
ResolvedQuestionPolicy resolve_question_policy(
    const std::string& configured_policy,  // 归一化后的配置值
    bool policy_explicit,                  // 配置或 CLI 显式指定过
    int configured_timeout_seconds,
    const std::string& permission_mode);   // ctx.current_permission_mode() 的返回值
```

优先级（高→低）：

1. **goal 无人值守** —— 不进本函数，工具入口现有 `goal_unattended(ctx)` 分支保持第一优先（返回 goal 专属文案）。
2. **显式配置**（config 或 CLI）—— 按面值使用。
3. **YOLO 隐式映射** —— `permission_mode == "yolo"` 且未显式配置 → Deny。
4. **默认** —— Ask。

进 `acecode_testable`，Node 依赖零，gtest 直接覆盖全部分支组合。

### D3. 策略经 `ToolContext` 注入，不让工具层碰 AppConfig

`ToolContext` 增加一个 `std::function<ResolvedQuestionPolicy()> question_policy` 探针（与 `goal_unattended_active` 同模式）。`AgentLoop` 组装 tool_ctx 时（`agent_loop.cpp:1770` 附近）绑定 lambda：内部调 `resolve_question_policy(loop_cfg_.question_policy, ..., current_permission_mode())`。空函数 = Ask（独立调用 ToolExecutor 的旧行为不变）。

- 备选：工具注册时闭包捕获 config —— 拒绝，TUI/daemon 的 `/model`、权限模式是会话内可变的，探针式 late-binding 才能反映实时权限模式（`/yolo` 切换后立即生效）。

### D4. Deny 返回 success=true 的自动应答，不是失败

沿用 `make_goal_unattended_ask_result` 的实证教训（success=false 会让模型当失败反复重问）。新增 `make_policy_denied_ask_result()`：

- `success = true`
- 文案：说明当前会话禁止交互提问、指示模型选推荐项或最合理假设、简述决策后继续推进。
- `metadata` 增加 `{"ask_user_question_auto": {"mode": "deny", "origin": "<origin>"}}`，TUI/Web 转录行可标注「已按策略自动应答」。

`待开发.md` 原文写的「可恢复错误」形态被否决：goal 无人值守上线后已验证 success=true 指令式应答不会让模型死循环，两者保持同构降低心智负担。

### D5. Timeout：自动采纳每个 question 的第一个选项

工具 description 已约定「推荐项排第一」，第一个选项即预选项。两路实现：

- **TUI**：`state.ask_cv.wait` 改 `wait_for` 循环（500ms 粒度）带 deadline；overlay 打开期间正常交互；到点后工具线程自己收 overlay（复用现有清理块）并合成 answers（每题第一选项 label）。overlay 顶部提示行显示「N 秒无操作将自动选择推荐项」（静态文案，不逐秒刷新）。
- **daemon**：`AskUserQuestionPrompter` 超时分支从「返回 cancelled=true」改为返回新增的 `timed_out=true` 标记（`AskUserQuestionResponse` 加字段；`cancelled` 保持 false）。JSON 桥（tool_executor.hpp 注释的 wire format）响应对象透传 `"timed_out": true`。async 工具收到后合成 first-option answers。`question_closed` 事件 reason 已有 `"timeout"` 值，前端 QuestionPicker 既有关闭逻辑复用，协议零新增。
- prompter timeout 注入点：`session_registry.cpp` 创建 entry 时按解析后的策略传 `std::chrono::seconds(timeout)`；策略非 Timeout 时传 0 维持无超时。**注意**：会话运行中经 `/yolo` 等切换权限模式不重建 prompter —— timeout 值以会话创建时为准，deny/ask 分支则实时生效（探针式）。此不对称记入 Risks。
- 用户超时后的答案不可再提交：`notify_response` 现有「未知 request_id = no-op」语义天然兜底。
- ToolResult `metadata.ask_user_question_auto = {"mode": "timeout", "seconds": N}`；`output` 前缀注明「用户 N 秒未回答，已自动采纳推荐项」，让模型知道这不是用户的真实意志。
- 备选：TUI 采纳当前焦点项（更贴 Claude Code「预选项」语义）—— 拒绝，多题流程下逐题焦点状态复杂、边际价值低；v1 统一第一选项，双端行为可预测。

### D6. CLI `--question-policy <ask|deny|timeout[:秒数]>`

- TUI：`src/cli/interactive_options.{hpp,cpp}` 解析（`timeout:120` 冒号语法），`src/main.cpp` 启动时覆写内存中 `cfg.agent_loop.question_policy` + 置 explicit 标记（不落盘），与 `--yolo` 模式一致。
- daemon：`src/daemon/cli.cpp` 同款参数，作用于 worker 的 cfg 副本，对该 daemon 全部会话生效。
- 非法值：启动报错退出（fail fast，与 `--resume` 未知 session 的处理风格一致）而非静默归一化 —— CLI 是显式意志，静默改写比报错更危险。

## Risks / Trade-offs

- **[timeout 自动选错项]** 推荐项不一定真是用户想要的 → 文案 + metadata 明确标注「自动采纳」，模型被告知可在后续被用户纠正；默认 policy 仍是 ask，timeout 是用户显式 opt-in。
- **[prompter timeout 会话中途不可变]** `/yolo` 运行中切换只影响 deny/ask 分支，timeout 秒数在会话创建时固化 → v1 记录为已知限制（改动 prompter 生命周期收益不成比例）；文档注明。
- **[YOLO 隐式 deny 改变既有 YOLO 会话行为]** 升级后 `--yolo` 用户的 AskUserQuestion 从弹窗变为自动应答 → 这正是 `待开发.md` 期望的语义修复；CHANGELOG/文档醒目标注，需要旧行为的用户显式写 `"question_policy": "ask"`。
- **[explicit 标记与 sparse-on-write 冲突]** 用户手写 `"question_policy": "ask"` 后 save_config 会因等于默认值而丢键，下次加载 explicit 丢失 → 接受：save_config 仅在 ACECode 内部改配置时发生（`/model --default` 等），这些路径不触碰 question_policy；如用户确要显式 ask + YOLO，文档建议 CLI `--question-policy ask`。
- **[TUI wait_for 500ms 轮询]** 相比纯 condvar 多了空转 → 与 prompter 既有 50ms 轮询同风格，500ms 粒度对秒级 timeout 足够，CPU 开销可忽略。

## Migration Plan

纯增量：默认 `question_policy="ask"` 时所有路径行为与现状 bit-for-bit 一致（TUI 仍无限期 wait，prompter 仍 timeout=0）。YOLO 隐式映射是唯一行为变化，随版本说明发布。回滚 = 配置 `"question_policy": "ask"` 或 CLI 覆盖，无数据迁移。

## Open Questions

（无 —— 待开发.md 列的研究方向已全部在 D1-D6 决策；「按运行入口区分默认值」被 YOLO 隐式映射 + CLI 覆盖组合替代，子代理经 fallback_permissions 继承父会话 YOLO 时自然获得 deny。）
