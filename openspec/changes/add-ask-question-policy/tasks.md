# Tasks: add-ask-question-policy

## 1. 配置层

- [x] 1.1 `src/config/config.hpp`：`AgentLoopConfig` 增加 `question_policy`（string，默认 "ask"）、`question_timeout_seconds`（int，默认 60）、`question_policy_explicit`（bool，默认 false，不序列化）三个字段，附注释说明语义与优先级
- [x] 1.2 `src/config/config.cpp` `load_config`：解析两个新键（非法 policy 归一化 "ask" + LOG_WARN；秒数 clamp [5,3600] + LOG_WARN；显式含 `question_policy` 键时置 explicit 标记）
- [x] 1.3 `src/config/config.cpp` `save_config`：sparse-on-write —— 仅非默认值时写出两个键，explicit 标记永不序列化
- [x] 1.4 config 单测：默认不落盘 / 非法值归一化 / clamp / explicit 标记 round-trip（tests/config/）

## 2. 策略解析纯函数层

- [x] 2.1 调整 `resolve_question_policy`：移除 permission mode 参数与 YOLO deny 映射，仅保留显式配置 > 默认 Ask
- [x] 2.2 更新策略单测：YOLO 与 default 使用同一 Ask 语义，显式 deny/timeout 按面值生效

## 3. ToolContext 注入与自动应答构造

- [x] 3.1 `src/tool/tool_executor.hpp`：`ToolContext` 增加 `std::function<ResolvedQuestionPolicy()> question_policy` 探针（空函数 = Ask），注释对齐 `goal_unattended_active` 模式
- [x] 3.2 `src/agent_loop.cpp`：提问策略不再读取当前 permission mode，移除 LOOP-YOLO 的 Deny 特例
- [x] 3.3 `src/tool/ask_user_question_tool.{hpp,cpp}`：新增 `make_policy_denied_ask_result(origin)`（success=true + 自行决策文案 + `metadata.ask_user_question_auto={mode:"deny",origin}`）与 `make_timeout_adopted_ask_result(questions, question_order, seconds)`（合成 first-option answers + output 前缀注明自动采纳 + metadata mode:"timeout"）
- [x] 3.4 移除 YOLO 工作区外首次写入确认门及其会话状态；显式 Deny 规则改为静默拒绝；补回归测试证明两条路径均不会调用 permission prompt

## 4. TUI 路径

- [x] 4.1 `create_ask_user_question_tool` execute：active goal 不再立即自动应答，而是将生效策略覆盖为 Timeout(30)
- [x] 4.2 ask overlay 渲染（src/tui/ 对应组件）：策略为 Timeout 时顶部加静态提示行「N 秒无操作将自动选择推荐项」（从 ask_payload 或新 TuiState 字段带入秒数）
- [x] 4.3 TUI 自动化回归：Timeout 到期会自动采纳推荐项并清理 overlay；active goal 显示 30 秒窗口且超时前回答按用户选择返回

## 5. Daemon 路径

- [x] 5.1 `AskUserQuestionPrompter::prompt` 增加 per-call timeout override，补单测确认 override 胜过构造时默认窗口
- [x] 5.2 `src/session/session_registry.cpp`：创建 entry 时解析策略，policy=Timeout 则以 `std::chrono::seconds(timeout)` 实例化 prompter，否则维持 0；`ask_user_questions` JSON 桥响应透传 `"timed_out": true`
- [x] 5.3 daemon async 路径：active goal 正常发 `question_request`，为该次 prompt 注入 30 秒 timeout，超时返回自动采纳结果
- [x] 5.4 `docs/daemon-api.md`：question_request/question_closed 一节补充 policy 语义与 reason="timeout" 行为说明

## 6. CLI 覆盖

- [x] 6.1 `src/cli/interactive_options.{hpp,cpp}`：解析 `--question-policy <value>`（含 `timeout:N` 冒号语法），非法值置错误标记；`src/main.cpp` 启动处应用覆盖（覆写 cfg.agent_loop + 置 explicit）或报错退出
- [x] 6.2 `src/daemon/cli.cpp`：daemon 入口同款参数与应用逻辑
- [x] 6.3 CLI 解析单测（tests/cli/）：合法三态 / 冒号秒数 / 非法值报错

## 7. 收尾验证

- [x] 7.1 Debug `acecode` / `acecode_unit_tests` 构建通过；除 5 个被当前运行中桌面实例占锁的 `DesktopSingleInstance` 用例外，完整测试集 2569/2569 通过，新增 TUI 两项再单独通过；Web `pnpm test` / `pnpm build` 通过
- [x] 7.2 行为回归确认：YOLO AskUserQuestion 仍弹 UI、YOLO 权限确认（含外部首次写与显式 Deny 规则）不弹 UI、goal 提问固定 30 秒且超时自动采纳
- [x] 7.3 更新 `CLAUDE.md`、daemon API 文档与 goal continuation prompt，移除「YOLO 禁止提问 / goal 禁止 AskUserQuestion」的旧契约
