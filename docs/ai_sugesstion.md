# AI Agent 趋势与 ACECode 后续开发建议

更新时间：2026-06-03

这份文档结合 2026 年前后主流 AI agent 方向和 ACECode 当前架构，给出后续值得投入的开发建议。它不是承诺路线图，而是用于后续 OpenSpec 拆分和排期讨论的候选池。

## 参考趋势

- OpenAI 的 Responses API 和 Agents SDK 正在把 agent 能力收敛到更结构化的运行时：模型请求、内置工具、MCP、审批、状态、沙箱、trace 和 eval 都逐渐成为一套工程化表面，而不只是一次 chat completion 调用。参考：[Responses API](https://platform.openai.com/docs/api-reference/responses?api-mode=responses)、[Agents SDK](https://developers.openai.com/api/docs/guides/agents)、[agent evals](https://developers.openai.com/api/docs/guides/agent-evals)。
- MCP 已经成为 agent 接外部工具、资源和提示模板的事实标准之一。它强调 host/client/server 边界、工具/资源/提示 primitives、能力协商、动态工具发现和 stdio/http 传输。参考：[MCP architecture](https://modelcontextprotocol.io/docs/learn/architecture)、[MCP client best practices](https://modelcontextprotocol.io/docs/develop/clients/client-best-practices)。
- A2A 开始补 MCP 的另一面：MCP 更像 agent 访问工具和上下文，A2A 更像 agent 与 agent 之间跨厂商、跨框架协作的协议。参考：[Google A2A announcement](https://developers.googleblog.com/en/a2a-a-new-era-of-agent-interoperability/)、[A2A specification](https://google-a2a.github.io/A2A/specification/)。
- LangGraph 等 runtime 的重点不是更花哨的 prompt，而是 durable execution、human-in-the-loop、长期状态、可恢复执行、可观测性。参考：[LangGraph overview](https://docs.langchain.com/oss/python/langgraph/overview)、[LangGraph persistence](https://docs.langchain.com/oss/python/langgraph/persistence)、[HITL middleware](https://docs.langchain.com/oss/python/langchain/human-in-the-loop)。
- OpenTelemetry 已经在定义 GenAI/agent 语义约定，说明 agent trace、tool call span、metrics、events 会成为生产级 agent 的基础能力。参考：[OpenTelemetry GenAI semantic conventions](https://opentelemetry.io/docs/specs/semconv/gen-ai/)。
- OWASP 开始把 agentic AI 和 skills 作为独立安全面来讨论，重点包括 skill 供应链、权限、沙箱、网络限制、审计、prompt injection 和工具滥用。参考：[OWASP Agentic AI threats and mitigations](https://genai.owasp.org/resource/agentic-ai-threats-and-mitigations/)、[OWASP Agentic Skills Top 10](https://owasp.org/www-project-agentic-skills-top-10/)。
- SWE-bench 系列说明 coding agent 的评价已经从“能否回答问题”转向“能否在真实仓库里稳定改代码、跑测试、产出 patch”，并且开始覆盖 multimodal 场景。参考：[SWE-bench](https://www.swebench.com/SWE-bench/)、[SWE-bench leaderboard](https://www.swebench.com/)。

## ACECode 当前适配点

ACECode 已经有不错的 agent 底座：C++17 共享 agent core、TUI/daemon/web/desktop 多表面、工具注册、权限提示、session 持久化、MCP、skills、memory、browser bridge、structured attachments、plan mode 和 goal 续跑机制。后续更值得做的不是再堆一个聊天 UI，而是把这些能力工程化成更可靠、更安全、可观测、可评估、可互操作的 agent runtime。

## 优先建议

### 1. MCP 工具渐进式发现与动态连接

建议优先级：P0

当前 ACECode 已支持 MCP 配置和工具注册，但未来用户连接几十个 MCP server 后，如果每轮都把所有工具 schema 放进模型上下文，会浪费 token、破坏 prompt cache，并降低工具选择质量。

建议开发：

- 新增 MCP capability catalog：记录 server、tool/resource/prompt 名称、描述、输入 schema 摘要、权限风险、启用状态、最近错误。
- 新增 `search_tools` / `inspect_tool` / `call_tool` 三层工具模型：模型先检索，再查看完整 schema，最后通过稳定 meta-tool 调用实际 MCP 工具。
- 支持按任务 lazy connect MCP server：启动时只读配置和摘要，真正需要时再连接 server。
- 处理 `notifications/tools/list_changed`：刷新本地索引，不强制重启 daemon。
- 给 Web 设置页加工具检索和启停视图，帮助用户理解当前 agent 可见能力。

适配代码方向：

- `src/tool/`：工具注册、meta-tool、MCP 调用代理。
- `src/web/handlers/` 和 `web/src/`：MCP catalog UI。
- `docs/user-manual.md`、`docs/daemon-api.md`：更新协议和用户说明。

候选 OpenSpec：`mcp-progressive-tool-discovery`

### 2. 安全的 code mode / programmatic tool calling

建议优先级：P0

多个工具串联时，把每一步中间结果都送回模型，会造成高延迟、高 token 和数据外泄风险。MCP 官方 best practices 已经把“模型写一段小脚本，沙箱执行工具链，只把最终摘要返回模型”作为可扩展模式。

建议开发：

- 提供受限脚本执行器：优先考虑 Wasm/QuickJS/独立 helper process，默认无网络、无文件系统直通。
- Host broker 暴露 typed stubs，例如 `read_file`、`grep`、`mcp.call`、`browser.read_page`，脚本只能通过 broker 调工具。
- 每个 brokered tool call 仍走 ACECode 现有 permission policy，批准脚本不等于批准脚本内部所有工具调用。
- 输出只允许结构化摘要、计数、artifact ref，避免把大文件或敏感内容直接回灌到模型上下文。
- 加资源限制：超时、内存、最大调用次数、最大输出大小。

适配代码方向：

- `src/tool/`：新增 `code_mode` 工具和 broker。
- `src/permissions.hpp`：增加 brokered call 的权限上下文。
- `src/session/`：记录脚本、子调用、审批和最终结果，便于 replay。

候选 OpenSpec：`sandboxed-code-mode-tools`

### 3. Agent trace、回放和 eval harness

建议优先级：P0

ACECode 后续会越来越依赖复杂工具链、浏览器、MCP、skills 和不同 provider。没有 trace 和 eval，很难判断一个 agent 改动是变好了还是只是“这次看起来能跑”。

建议开发：

- 定义统一 trace event schema：`model_call`、`tool_call`、`permission_request`、`permission_decision`、`context_budget`、`provider_error`、`retry`、`compact`、`goal_state`、`browser_action`。
- 本地写 JSONL trace，并提供 daemon API 查询和 Web trace viewer。
- 按 OpenTelemetry GenAI semantic conventions 增加可选 OTel exporter，先做 spans/metrics，不必一次性做全。
- 做 ACECode 自己的 eval runner：临时 repo、脚本化用户输入、预设权限响应、可替换 stub provider、命令行跑多组任务。
- 建立小型 ACE-bench：覆盖文件编辑、测试修复、MCP 调用、browser bridge、plan/goal、权限拒绝恢复、中文任务。

适配代码方向：

- `src/agent_loop.cpp`：trace 事件源。
- `src/session/`：trace 存储和 replay。
- `src/web/`、`web/src/`：trace 查看。
- `tests/` 或 `scripts/`：eval harness。

候选 OpenSpec：`agent-trace-and-eval-harness`

### 4. Durable workflow runtime，把 `/goal` 和 plan mode 升级成可恢复任务流

建议优先级：P1

ACECode 已经有 `/goal` 和 plan mode，但长任务仍主要依赖 agent loop 自己连续跑。后续可以把长任务显式建模成可恢复 workflow：计划、执行、验证、等待用户、失败恢复、继续。

建议开发：

- 引入 workflow state machine：`planning`、`awaiting_approval`、`executing`、`verifying`、`blocked`、`complete`。
- 每个阶段有 checkpoint，daemon 或 desktop 崩溃后能从阶段边界恢复。
- 支持用户可见的 pause/resume/drain/cancel，而不是只靠中断当前 provider 请求。
- 把 plan 文件、goal 状态、任务 checklist、验证命令和 trace 关联起来。
- Web/TUI 显示同一份 workflow 状态，避免多表面状态不一致。

适配代码方向：

- `src/session/`：持久化 workflow 状态。
- `src/agent_loop.cpp`：阶段调度。
- `src/commands/`：扩展 `/goal`、`/plan`、`/workflow`。
- `web/src/components/StatusBar.jsx`：状态入口。

候选 OpenSpec：`durable-agent-workflow-runtime`

### 5. 权限和风险引擎 2.0

建议优先级：P1

AI coding agent 的安全边界不应只停留在“读工具自动、写工具询问、yolo 全放开”。工具越多，权限就越需要能表达路径、网络、凭据、外部系统、数据流和长期授权。

建议开发：

- 给每个工具声明 risk metadata：读、写、执行、网络、浏览器、外部 API、凭据访问、不可逆操作。
- 支持 path/domain/server 级 allowlist 和 denylist，例如只允许浏览器访问某些域名，只允许 MCP server 调某些工具。
- 权限弹窗支持 approve/edit/reject/respond：用户可编辑工具参数后再批准。
- Persistent grant 必须有作用域和过期条件，例如本 session、当前 workflow、当前目录、指定工具参数模式。
- 检测可疑数据流：从不可信网页/文档读取的数据直接进入 shell、git credential、HTTP upload 等路径时提高风险等级。

适配代码方向：

- `src/permissions.hpp`：风险模型和 grant scope。
- `src/tool/`：工具定义附带风险 metadata。
- `web/src/components/PermissionModal.jsx`、TUI confirm：展示参数 diff 和风险解释。

候选 OpenSpec：`permission-risk-engine-v2`

### 6. Skills 供应链治理

建议优先级：P1

ACECode 已有 skills 系统。趋势上 skills 是 agent 的行为层，风险不只来自代码，还来自 `SKILL.md` 里的自然语言指令、依赖、日志投毒和持久化记忆污染。

建议开发：

- 给 skill 增加 manifest：版本、作者、来源、hash、声明权限、依赖、需要的 MCP server、是否允许 shell/网络。
- 首次安装或启用时展示权限摘要，并把用户决定持久化。
- 支持 lockfile：固定 skill 版本和 hash，避免自动更新带来供应链风险。
- 做静态扫描：危险指令、敏感路径、网络 exfiltration 模式、要求读取 token/ssh/env 的描述。
- skill 运行时隔离：skill 只能影响自己的上下文和声明工具，不应默认扩展全局权限。

适配代码方向：

- `src/skills/`：manifest、hash、enable policy。
- `src/tool/skills_tool.cpp`、`skill_view_tool.cpp`：展示权限和来源。
- `web/src/components/SkillsPanel.jsx`：安装、启用、审计 UI。

候选 OpenSpec：`skills-supply-chain-governance`

### 7. Browser/computer-use 能力标准化

建议优先级：P1

ACECode 的 browser bridge 已经走 direct CDP、结构化 `read-page`、截图、网络和 devtools。下一步可以把它提升成兼容“computer use”范式的通用 UI 行动层，同时保留结构化 DOM 优先的优势。

建议开发：

- 定义 browser action protocol：`read_page`、`screenshot`、`click`、`type`、`drag`、`wait`、`assert`、`network_assert`、`export_artifact`。
- 每个 action 都写 trace，支持失败后 replay。
- 加视觉断言：截图区域、元素截图、像素/文本/布局检查，服务前端任务。
- 支持 provider computer tool adapter：当模型支持 computer tool 时，ACECode 提供隔离浏览器环境和截图回传；不支持时继续用结构化 CLI 路径。
- 默认隔离 profile、domain allowlist、敏感站点确认，避免 agent 在真实登录态里误操作。

适配代码方向：

- `ace-browser-host/`、`ace-browser-bridge/`：action 协议和 trace。
- `src/tool/ace_browser_bridge/`：工具包装和权限。
- `docs/ace-browser-bridge.md`：使用模型和安全边界。

候选 OpenSpec：`browser-computer-use-runtime`

### 8. 文档和多模态工作区能力

建议优先级：P1

Coding agent 的输入正在从纯文本 issue 变成截图、PDF、Word、Excel、网页附件和日志包。ACECode 已有 structured attachments 和输出图片显示，可以继续往“文档摄取管线”发展。

建议开发：

- 新增 `document_intake` 工具：识别 PDF/DOCX/XLSX/PPTX/image/text/log，输出统一 content parts、摘要、页码/单元格/坐标 provenance。
- PDF 走文本抽取加 OCR fallback；Office 文档走结构化解析；图片走 OCR 和视觉描述。
- 建立本地 artifact cache，provider 只拿必要摘要和引用，避免把整个文档塞进上下文。
- 支持从 browser `read-page` 的 attachments 导出后直接进入 document pipeline。
- Web/desktop 中给附件加预览、来源、提取状态和引用跳转。

适配代码方向：

- `src/tool/`：document intake 工具。
- `src/session/`：artifact/cache 元数据。
- `web/src/components/AttachmentStrip.jsx`：预览和 provenance。

候选 OpenSpec：`document-intake-pipeline`

### 9. A2A 预览接口，让 ACECode 既能调用 agent，也能被调用

建议优先级：P2

MCP 解决“agent 调工具”，A2A 解决“agent 调 agent”。短期不建议做复杂 multi-agent swarm，但可以先做最小互操作层，为企业流程集成留接口。

建议开发：

- daemon 暴露 `/.well-known/agent-card.json` 或等价 discovery endpoint：描述 ACECode 名称、能力、输入输出 modality、认证方式、任务端点。
- 新增任务 API：创建任务、查询状态、订阅事件、取消任务、取 artifact。
- 让 ACECode 能把某些子任务委托给远端 A2A agent，并把远端 trace/artifact 作为普通 tool result 纳入 session。
- 权限上区分 local tools、MCP tools、remote agent delegation。

适配代码方向：

- `src/web/handlers/`：agent card 和 task API。
- `src/tool/`：A2A delegate tool。
- `src/session/`：remote task state 和 artifact。

候选 OpenSpec：`a2a-agent-endpoint-preview`

### 10. 项目知识图谱和上下文预算管理

建议优先级：P2

ACECode 已有 memory、project instructions、skills 和 session compact。下一步可以把它们统一成上下文 broker：知道什么时候该拿文件、符号、历史决策、测试结果和用户偏好。

建议开发：

- 建本地 project index：文件树、语言、符号、测试入口、OpenSpec change、最近失败命令。
- 语义检索和符号检索分层：先定位模块，再读文件，减少全仓库 grep。
- memory 增加来源、时间、置信度、作用域和过期策略，降低 memory poisoning 风险。
- prompt cache 友好：稳定 system/tool 前缀，把动态上下文放到后段。
- 在 trace 中记录每轮上下文预算：哪些内容进入了模型，估计 token，为什么选它。

适配代码方向：

- `src/memory/`、`src/project_instructions/`：metadata 和作用域。
- `src/session/`：context snapshot。
- `src/tool/`：project index 查询工具。

候选 OpenSpec：`context-broker-and-project-index`

### 11. 本地 SWE-bench 风格任务集和 CI/PR agent

建议优先级：P2

ACECode 是 coding agent，最有价值的增长应该能落到“真实仓库任务完成率”。可以先不追公开 SWE-bench 分数，而是建立自己的可复现任务集。

建议开发：

- 任务规格：初始仓库状态、用户需求、允许工具、期望测试、禁止修改路径、验收命令。
- eval runner 自动复制临时工作区、启动 ACECode、注入用户消息、模拟审批、运行验证命令、收集 patch。
- Web/desktop 增加“从 issue 开始”的工作流：创建分支、实现、跑测试、生成总结、准备 commit/PR。
- 建立小型回归集：每次 provider、工具、权限、prompt、browser 改动都跑核心任务。

适配代码方向：

- `scripts/`：eval runner。
- `tests/`：可复用 stub provider 和 session fixture。
- `docs/`：任务格式和贡献说明。

候选 OpenSpec：`local-agent-benchmark-suite`

### 12. Provider 层升级到 Responses-style item model

建议优先级：P2

ACECode 当前以 OpenAI-compatible Chat Completions 为主要抽象。长期看，agent 事件会越来越像 item stream：message、tool call、tool output、reasoning item、file/image input、computer call、background response、previous response 等。继续只用 chat message 抽象会限制 provider 能力。

建议开发：

- 在内部定义 provider-neutral item model：text、image、file、tool_call、tool_result、reasoning_ref、computer_call、artifact_ref。
- OpenAI Responses、Chat Completions、Copilot、其他 OpenAI-compatible provider 都映射到这套内部结构。
- Session JSONL 继续兼容旧消息，但新字段使用 structured content parts。
- 对 provider 能力做显式 capability routing：vision、file input、computer、web search、function tools、background。

适配代码方向：

- `src/provider/`：item stream adapter。
- `src/session/`：兼容序列化。
- `src/tool/`：tool result attachment 和 artifact ref。

候选 OpenSpec：`provider-item-model-upgrade`

## 建议排期

第一阶段，先做可观测和可控：

1. `agent-trace-and-eval-harness`
2. `mcp-progressive-tool-discovery`
3. `permission-risk-engine-v2`

第二阶段，做长任务能力：

1. `durable-agent-workflow-runtime`
2. `sandboxed-code-mode-tools`
3. `skills-supply-chain-governance`

第三阶段，拓展输入输出和互操作：

1. `document-intake-pipeline`
2. `browser-computer-use-runtime`
3. `a2a-agent-endpoint-preview`
4. `provider-item-model-upgrade`

## 暂时不建议优先做的方向

- 不建议先做“多 agent swarm UI”。没有 trace、权限、eval 和任务状态机，多 agent 只会放大不可控性。
- 不建议把 browser/computer-use 直接接到真实个人浏览器全权限 profile。应该先有隔离 profile、domain allowlist、action trace 和高风险确认。
- 不建议把所有 MCP/skills 一次性塞进系统 prompt。应先做渐进式工具发现和 catalog。
- 不建议依赖单一 provider 的私有能力来重写核心架构。可以支持 Responses-style 能力，但内部应保持 provider-neutral。

## 最小可执行下一步

如果只选一个最值得马上启动的 change，建议先做 `agent-trace-and-eval-harness`。原因是它会直接提高后续所有改动的验证质量，并能让 MCP、browser、skills、goal、plan、provider 变更都留下可对比证据。

如果想选一个用户可感知最强的 change，建议做 `mcp-progressive-tool-discovery`。它能让 ACECode 更自然地接入大量外部工具，同时减少上下文污染和工具选择错误。
