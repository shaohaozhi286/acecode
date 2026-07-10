# Proposal: add-ask-question-policy

## Why

YOLO / 无人值守场景下 `AskUserQuestion` 仍会无限期阻塞等待用户回答（`待开发.md` 21-44 行挂账问题）：`--yolo` 只跳过工具权限确认，模型一旦调 `AskUserQuestion`，TUI 弹 overlay、daemon 发 `question_request` 后 turn 就挂住，headless CLI / 定时任务 / 后台 daemon / 子代理场景没有人来回答。当前唯一豁免是 goal 无人值守模式（`goal_unattended_active()` 路径自动应答），非 goal 的无人值守没有任何保障。Claude Code v2.1.200 的 `askUserQuestionTimeout` 设计（默认 never、可配置 idle timeout 自动采纳预选项）验证了这一能力的必要性与合理形态。

## What Changes

- `config.agent_loop` 新增 `question_policy`（`"ask"` 默认 / `"deny"` / `"timeout"`）与 `question_timeout_seconds`（policy=timeout 时生效，默认 60）两个字段，遵循 sparse-on-write。
- 新增共享策略解析层（纯函数）：goal 无人值守 > 显式配置 > YOLO 隐式映射 deny > 默认 ask；TUI 与 daemon 两路 `AskUserQuestion` 实现统一走它。
- `deny`：不弹任何 UI，立即返回自动应答 ToolResult（沿用 goal 无人值守的 success=true + 「自行决策并继续」文案模式），metadata 标注 auto-answer 来源。
- `timeout`：等待 N 秒无人回答后自动采纳每个 question 的第一个选项（工具描述约定推荐项排第一）并继续；TUI overlay 与 daemon `AskUserQuestionPrompter` 各自实现倒计时关闭。
- YOLO 模式（含 `--dangerous`）在用户未显式配置 `question_policy` 时默认映射为 `deny`；显式配置永远胜过隐式映射。
- TUI 与 daemon CLI 新增 `--question-policy <ask|deny|timeout[:秒数]>` 启动参数，session 级覆盖配置（不落盘），与 `--yolo` 的内存覆盖模式一致。

## Capabilities

### New Capabilities

- `ask-question-policy`: AskUserQuestion 的应答策略（ask/deny/timeout）——策略解析优先级、deny 自动应答契约、timeout 自动采纳契约、YOLO 隐式映射、CLI 覆盖、双端（TUI/daemon）一致性、前端 pending question 清理。

### Modified Capabilities

（无 —— 现有 spec 目录下没有 AskUserQuestion 相关 spec；goal 无人值守自动应答行为保持不变，仍是最高优先级豁免。）

## Impact

- **Config**: `src/config/config.hpp`（`AgentLoopConfig` 加字段）、`src/config/config.cpp`（load/save + 校验 + explicit 标记）。
- **策略层**: 新增 `src/tool/question_policy.{hpp,cpp}`（纯函数，进 `acecode_testable`）。
- **工具层**: `src/tool/ask_user_question_tool.cpp` TUI 与 async 两个 execute 路径接策略分支；`ToolContext` 增加策略注入字段（`src/tool/tool_executor.hpp`）。
- **AgentLoop**: `src/agent_loop.cpp` 在构造 tool_ctx 处注入解析后的策略（复用已有 `loop_cfg_` / `current_permission_mode` / `goal_unattended_active`）。
- **Daemon**: `src/session/ask_user_question_prompter.{hpp,cpp}`（timeout 语义从「取消」扩展为「标记 timed_out」）、`src/session/session_registry.cpp`（按策略注入 prompter timeout）、`src/daemon/cli.cpp`（CLI 参数）。
- **TUI**: `ask_user_question_tool.cpp` 的 condvar 等待改带 deadline；`src/cli/interactive_options.{hpp,cpp}` + `src/main.cpp`（CLI 参数）。
- **前端**: 已有 `question_closed`（reason 字段）事件通路，超时关闭复用，无协议新增；`docs/daemon-api.md` 补充 reason=timeout 语义说明。
- **测试**: 策略解析纯函数单测、prompter timeout 单测、config round-trip 单测。
