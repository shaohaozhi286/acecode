# Tasks: add-ask-question-policy

## 1. 配置层

- [x] 1.1 `src/config/config.hpp`：`AgentLoopConfig` 增加 `question_policy`（string，默认 "ask"）、`question_timeout_seconds`（int，默认 60）、`question_policy_explicit`（bool，默认 false，不序列化）三个字段，附注释说明语义与优先级
- [x] 1.2 `src/config/config.cpp` `load_config`：解析两个新键（非法 policy 归一化 "ask" + LOG_WARN；秒数 clamp [5,3600] + LOG_WARN；显式含 `question_policy` 键时置 explicit 标记）
- [x] 1.3 `src/config/config.cpp` `save_config`：sparse-on-write —— 仅非默认值时写出两个键，explicit 标记永不序列化
- [x] 1.4 config 单测：默认不落盘 / 非法值归一化 / clamp / explicit 标记 round-trip（tests/config/）

## 2. 策略解析纯函数层

- [x] 2.1 新建 `src/tool/question_policy.{hpp,cpp}`：`QuestionPolicy` 枚举、`ResolvedQuestionPolicy` 结构、`resolve_question_policy(configured_policy, policy_explicit, configured_timeout_seconds, permission_mode)` 纯函数（显式配置 > yolo 隐式 Deny > 默认 Ask），加入 `acecode_testable` 源列表（CMakeLists.txt）
- [x] 2.2 单测 `tests/tool/question_policy_test.cpp`：全分支组合（显式 ask+yolo / 隐式 yolo→deny / timeout 面值 / default→ask / origin 字段正确性）

## 3. ToolContext 注入与自动应答构造

- [x] 3.1 `src/tool/tool_executor.hpp`：`ToolContext` 增加 `std::function<ResolvedQuestionPolicy()> question_policy` 探针（空函数 = Ask），注释对齐 `goal_unattended_active` 模式
- [x] 3.2 `src/agent_loop.cpp`（~1770 tool_ctx 组装处）：绑定探针 lambda —— 内部调 `resolve_question_policy(loop_cfg_.question_policy, loop_cfg_.question_policy_explicit, loop_cfg_.question_timeout_seconds, <current_permission_mode 同款逻辑>)`
- [x] 3.3 `src/tool/ask_user_question_tool.{hpp,cpp}`：新增 `make_policy_denied_ask_result(origin)`（success=true + 自行决策文案 + `metadata.ask_user_question_auto={mode:"deny",origin}`）与 `make_timeout_adopted_ask_result(questions, question_order, seconds)`（合成 first-option answers + output 前缀注明自动采纳 + metadata mode:"timeout"）

## 4. TUI 路径

- [x] 4.1 `create_ask_user_question_tool` execute：goal_unattended 分支之后插入策略分支 —— Deny 立即返回 3.3 结果；Timeout 时 `state.ask_cv.wait` 改为 500ms `wait_for` 循环带 deadline，到期收 overlay（复用现有清理块）并返回自动采纳结果
- [x] 4.2 ask overlay 渲染（src/tui/ 对应组件）：策略为 Timeout 时顶部加静态提示行「N 秒无操作将自动选择推荐项」（从 ask_payload 或新 TuiState 字段带入秒数）
- [ ] 4.3 TUI 手测：`--question-policy timeout:10` 下触发 AskUserQuestion，验证 10 秒自动采纳与超时前正常回答两条路径

## 5. Daemon 路径

- [x] 5.1 `src/session/ask_user_question_prompter.{hpp,cpp}`：`AskUserQuestionResponse` 增加 `timed_out` 字段；超时分支返回 `timed_out=true`（cancelled 保持 false），`question_closed` reason 维持 "timeout"；补 prompter 单测（超时返回 timed_out / 超时后 notify_response no-op）
- [x] 5.2 `src/session/session_registry.cpp`：创建 entry 时解析策略，policy=Timeout 则以 `std::chrono::seconds(timeout)` 实例化 prompter，否则维持 0；`ask_user_questions` JSON 桥响应透传 `"timed_out": true`
- [x] 5.3 `create_ask_user_question_tool_async` execute：goal_unattended 之后插入 Deny 分支；响应含 `timed_out=true` 时返回自动采纳结果（不再走 cancelled→rejected）
- [x] 5.4 `docs/daemon-api.md`：question_request/question_closed 一节补充 policy 语义与 reason="timeout" 行为说明

## 6. CLI 覆盖

- [x] 6.1 `src/cli/interactive_options.{hpp,cpp}`：解析 `--question-policy <value>`（含 `timeout:N` 冒号语法），非法值置错误标记；`src/main.cpp` 启动处应用覆盖（覆写 cfg.agent_loop + 置 explicit）或报错退出
- [x] 6.2 `src/daemon/cli.cpp`：daemon 入口同款参数与应用逻辑
- [x] 6.3 CLI 解析单测（tests/cli/）：合法三态 / 冒号秒数 / 非法值报错

## 7. 收尾验证

- [ ] 7.1 全量构建 + `acecode_unit_tests` 通过（BUILD_TESTING=ON）
- [x] 7.2 行为回归确认：默认配置下 TUI/daemon AskUserQuestion 行为与改动前一致（Ask 无限期等待、prompter timeout=0）；goal 无人值守路径不受影响
- [x] 7.3 更新 `CLAUDE.md`（Agent Loop And Tools 节补 question_policy 一句话）与 `docs/待开发.md`（勾掉 21-44 行挂账项，注明 change 名）
