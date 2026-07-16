# Proposal: add-ask-question-policy

## Why

`AskUserQuestion` 是业务澄清通道，工具权限确认是执行安全通道，两者不应被 YOLO 模式捆绑。YOLO 应当只跳过权限确认，模型仍可通过 `AskUserQuestion` 向正在观看会话的用户提问。另一方面，active `/goal` 会话需要持续推进，但不应立即吞掉有价值的澄清问题；它应给用户 30 秒回答窗口，超时后再自动采纳每题的推荐项。

## What Changes

- `config.agent_loop` 新增 `question_policy`（`"ask"` 默认 / `"deny"` / `"timeout"`）与 `question_timeout_seconds`（policy=timeout 时生效，默认 60）两个字段，遵循 sparse-on-write。
- 新增共享策略解析层（纯函数）：显式配置 > 默认 ask；YOLO 不参与提问策略解析。
- `deny`：不弹任何 UI，立即返回 success=true 的「自行决策并继续」自动应答，metadata 标注 auto-answer 来源。
- `timeout`：等待 N 秒无人回答后自动采纳每个 question 的第一个选项（工具描述约定推荐项排第一）并继续；TUI overlay 与 daemon `AskUserQuestionPrompter` 各自实现倒计时关闭。
- YOLO 模式（含 `--dangerous`）仍可正常弹出 AskUserQuestion，但工具执行的权限确认继续全部自动放行；工作区外首次写入也不再追加确认弹窗。
- active `/goal` 下 AskUserQuestion 强制使用 30 秒 timeout：正常弹 UI，超时后自动采纳每题第一个（推荐）选项；goal 的工具权限仍自动放行。
- TUI 与 daemon CLI 新增 `--question-policy <ask|deny|timeout[:秒数]>` 启动参数，session 级覆盖配置（不落盘），与 `--yolo` 的内存覆盖模式一致。

## Capabilities

### New Capabilities

- `ask-question-policy`: AskUserQuestion 的应答策略（ask/deny/timeout）——策略解析优先级、deny 自动应答契约、timeout 自动采纳契约、YOLO 解耦、goal 30 秒覆盖、CLI 覆盖、双端一致性。

### Modified Capabilities

（无 —— 现有 spec 目录下没有 AskUserQuestion 相关 spec。）

## Impact

- **Config**: `src/config/config.hpp`（`AgentLoopConfig` 加字段）、`src/config/config.cpp`（load/save + 校验 + explicit 标记）。
- **策略层**: 新增 `src/tool/question_policy.{hpp,cpp}`（纯函数，进 `acecode_testable`）。
- **工具层**: `src/tool/ask_user_question_tool.cpp` TUI 与 async 两个 execute 路径接策略分支；`ToolContext` 增加策略注入字段（`src/tool/tool_executor.hpp`）。
- **AgentLoop**: `src/agent_loop.cpp` 在构造 tool_ctx 处注入解析后的策略（复用已有 `loop_cfg_` / `goal_unattended_active`，不读取 permission mode）。
- **Daemon**: `src/session/ask_user_question_prompter.{hpp,cpp}`（timeout 语义从「取消」扩展为「标记 timed_out」）、`src/session/session_registry.cpp`（按策略注入 prompter timeout）、`src/daemon/cli.cpp`（CLI 参数）。
- **TUI**: `ask_user_question_tool.cpp` 的 condvar 等待改带 deadline；`src/cli/interactive_options.{hpp,cpp}` + `src/main.cpp`（CLI 参数）。
- **前端**: 已有 `question_closed`（reason 字段）事件通路，超时关闭复用，无协议新增；`docs/daemon-api.md` 补充 reason=timeout 语义说明。
- **测试**: 策略解析纯函数单测、prompter timeout 单测、config round-trip 单测。
