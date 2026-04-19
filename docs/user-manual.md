# ACECode 使用手册

ACECode 是一个运行在终端里的 AI 编程助手，基于 FTXUI 提供交互式 TUI 界面，支持多轮对话和自动工具调用。

---

## 目录

1. [快速开始](#1-快速开始)
2. [启动参数](#2-启动参数)
3. [首次配置](#3-首次配置)
4. [界面说明](#4-界面说明)
5. [基本交互](#5-基本交互)
6. [键盘快捷键](#6-键盘快捷键)
7. [斜杠命令](#7-斜杠命令)
8. [Shell 直通模式](#8-shell-直通模式)
9. [工具调用与权限确认](#9-工具调用与权限确认)
10. [权限模式](#10-权限模式)
11. [会话管理](#11-会话管理)
12. [Skills（技能扩展）](#12-skills技能扩展)
13. [MCP 服务器](#13-mcp-服务器)
14. [配置文件详解](#14-配置文件详解)
15. [常见问题](#15-常见问题)

---

## 1. 快速开始

```bash
# 直接启动，进入交互界面
./acecode

# 首次使用，先运行配置向导
./acecode configure
```

启动后，在底部输入框中输入你的问题或指令，按 `Enter` 发送即可。

---

## 2. 启动参数

| 参数 | 说明 |
|------|------|
| _(无参数)_ | 启动新会话 |
| `configure` | 运行配置向导（交互式设置 provider、model、API key 等） |
| `--resume` | 恢复最近一次会话 |
| `--resume <session-id>` | 恢复指定 ID 的历史会话 |
| `-dangerous` | 危险模式：绕过所有权限确认，自动执行所有工具调用（谨慎使用） |

**示例：**

```bash
./acecode --resume                   # 恢复最近会话
./acecode --resume abc123            # 恢复 ID 为 abc123 的会话
./acecode -dangerous                 # 危险模式启动，无需任何确认
```

---

## 3. 首次配置

运行 `./acecode configure` 启动配置向导，按提示选择：

1. **Provider（服务商）**
   - `copilot` — 使用 GitHub Copilot（需要有效的 Copilot 订阅）
   - `openai` — 使用兼容 OpenAI 接口的 API（本地模型、云端端点等）

2. **模型名称**
   - Copilot：如 `gpt-4o`、`claude-3.5-sonnet` 等
   - OpenAI 兼容：填写你的 API 端点支持的模型名

3. **API 配置**（仅 openai provider 需要）
   - Base URL：API 地址，如 `http://localhost:1234/v1`
   - API Key：鉴权密钥

配置保存在 `~/.acecode/config.json`，之后启动无需重复配置。

### GitHub Copilot 认证

首次使用 Copilot provider 时，acecode 会自动启动 Device Flow 认证：

1. 界面显示一个验证码（如 `ABCD-1234`）和验证网址
2. 在浏览器中打开该网址，输入验证码并授权
3. 认证成功后，token 会自动保存，后续启动无需重复操作

---

## 4. 界面说明

```
+--------------------------------------------------+
|  acecode v1.x.x  |  /path/to/your/project       |
+--------------------------------------------------+
|                                                  |
|  [user]   你好，帮我写一个 hello world            |
|                                                  |
|  [assistant] 好的，这是一个 Python 的...          |
|                                                  |
|  [tool]   file_write: hello.py                   |
|  [system] File written successfully              |
|                                                  |
+--------------------------------------------------+
|  [copilot] model: gpt-4o     tokens: 1.2k/128k  |
+--------------------------------------------------+
|  > 你的输入...                                    |
+--------------------------------------------------+
```

| 区域 | 说明 |
|------|------|
| **顶栏** | 显示版本号和当前工作目录 |
| **对话区** | 多轮对话内容，可滚动查看 |
| **底部状态栏** | 当前 provider、模型名称及 token 用量 |
| **输入框** | 输入你的消息，支持自动换行 |

**消息角色颜色：**
- `[user]` — 你发送的消息
- `[assistant]` — AI 的回复
- `[tool]` — 工具调用的执行详情
- `[system]` — acecode 系统提示（命令结果、错误信息等）

---

## 5. 基本交互

### 发送消息

在底部输入框中输入内容，按 `Enter` 发送。AI 会流式输出响应，你会看到文字逐渐出现。

### 等待和取消

- AI 思考或执行工具时，状态栏会显示"Thinking..."动画
- 按 `Escape` 可以中止当前正在进行的 AI 回复或工具执行

### 队列发送

如果 AI 还在处理上一条消息，你可以继续输入并发送，消息会进入队列，等 AI 空闲后自动发出。

---

## 6. 键盘快捷键

| 按键 | 功能 |
|------|------|
| `Enter` | 发送消息 / 确认选择 |
| `Escape` | 取消 AI 响应（等待时） / 退出 Shell 模式 / 拒绝工具确认 |
| `Ctrl+C` | 连按两次（1.2 秒内）退出程序 |
| `Page Up` | 对话区向上滚动 5 条 |
| `Page Down` | 对话区向下滚动 5 条 |
| `Home` | 滚动到对话区最顶部 |
| `End` | 滚动到对话区最底部（跟随最新消息） |
| `Up` / `Ctrl+P` | 输入历史回调（上一条） / 斜杠下拉菜单向上 |
| `Down` / `Ctrl+N` | 输入历史回调（下一条） / 斜杠下拉菜单向下 |
| `Tab` | 补全斜杠命令（下拉菜单中选中后确认） |
| `Ctrl+P` | 在空闲状态下循环切换权限模式 |

---

## 7. 斜杠命令

在输入框中以 `/` 开头输入命令，会自动弹出补全下拉菜单（支持按名称或描述模糊匹配）。

### 内置命令

| 命令 | 说明 |
|------|------|
| `/help` | 显示所有可用命令列表 |
| `/clear` | 清空当前对话历史（开始全新对话） |
| `/compact` | 压缩对话历史（保留摘要，减少 token 占用） |
| `/model` | 显示当前使用的模型名称 |
| `/model <name>` | 切换到指定模型（如 `/model gpt-4o`） |
| `/config` | 显示当前配置信息（provider、模型、上下文窗口等） |
| `/tokens` | 显示本次会话的 token 用量 |
| `/resume` | 显示会话选择器，恢复历史会话 |
| `/mcp` | 列出 MCP 服务器及状态 |
| `/mcp list` | 列出所有 MCP 工具（按服务器分组） |
| `/mcp enable <name>` | 连接一个已禁用或连接失败的服务器 |
| `/mcp disable <name>` | 停止服务器并注销其工具 |
| `/mcp reconnect <name>` | 强制断开并重新连接服务器 |
| `/mcp help` | 显示 /mcp 子命令帮助 |
| `/skills` | 列出所有已安装的技能 |
| `/skills reload` | 重新扫描磁盘，加载新增或修改的技能 |
| `/skills help` | 显示技能系统使用说明 |
| `/exit` | 退出 acecode |

### 斜杠下拉菜单

输入 `/` 后，界面会在输入框上方弹出补全下拉菜单：

- 最多显示 8 个匹配项，优先匹配前缀，其次匹配名称或描述中的子串
- 用 `Up/Down` 或 `Ctrl+P/Ctrl+N` 上下选择
- `Enter` 或 `Tab` 确认选择（填入命令名，不会立即提交）
- `Escape` 关闭下拉菜单

---

## 8. Shell 直通模式

Shell 直通模式让你直接在 acecode 中执行 shell 命令，绕过 LLM，结果立即显示。

### 进入方式

在**空输入框**中输入 `!`，输入框提示符会变为 `! >`，表示进入 Shell 模式。

### 退出方式

- 按 `Escape` 退出 Shell 模式并清空输入
- 在空 Shell 输入框中按 `Backspace` 也会退出 Shell 模式

### 执行命令

在 Shell 模式下输入命令并按 `Enter`，命令会通过内置 bash 工具执行，输出结果会显示在对话区。

**示例：**
```
! > ls -la
! > git status
! > npm test
```

> 注意：Shell 模式下执行的命令同样受权限模式约束（除非是 Yolo 模式）。

---

## 9. 工具调用与权限确认

当 AI 决定调用某个工具（如写文件、执行命令等），acecode 会暂停并在界面底部显示确认提示：

```
[bash] command: rm -f temp.txt
yes / always / no: _
```

### 响应选项

| 输入 | 含义 |
|------|------|
| `y` 或 `yes` | **允许**本次执行 |
| `a` 或 `always` | **永久允许**此工具后续无需再次确认（本次会话内有效） |
| `n` 或其他 | **拒绝**此次执行 |
| `Escape` | 拒绝此次执行 |

### 只读工具无需确认

以下工具默认自动执行，不会弹出确认框：
- `file_read` — 读取文件内容
- `grep` — 搜索文件
- `glob` — 查找文件路径

### 安全保护

acecode 内置了若干安全规则，以下操作会被自动拒绝，即使在 Yolo 模式下也有保护：
- 写入 `.env` 文件
- 写入 `.git/` 目录
- 执行 `rm -rf /`

---

## 10. 权限模式

acecode 有三种权限模式，控制工具调用时的确认行为。

| 模式 | 说明 |
|------|------|
| **Default（默认）** | 读操作自动允许；写/执行类工具需要确认 |
| **AcceptEdits** | 读操作、文件写入和文件编辑自动允许；bash 执行仍需确认 |
| **Yolo** | 所有工具调用自动允许，无任何确认（危险！） |

### 切换方式

- **TUI 中**：在空闲状态（非等待/非确认时）按 `Ctrl+P` 循环切换
- **命令行**：启动时加 `-dangerous` 参数直接进入 Yolo 模式

切换后，对话区会显示当前模式名称和描述。

---

## 11. 会话管理

acecode 会自动保存每次对话的完整历史，方便后续恢复。

### 会话自动保存

每次退出（无论是正常退出还是 Ctrl+C）时，会话都会自动保存。退出时终端会打印：

```
acecode: session <id> saved. Resume with: acecode --resume <id>
```

### 恢复会话

**方式一：命令行参数**
```bash
./acecode --resume           # 恢复最近一次会话
./acecode --resume <id>      # 恢复指定 ID 的会话
```

**方式二：TUI 内恢复**
1. 输入 `/resume` 命令
2. 在弹出的会话选择器中用 `Up/Down` 选择历史会话
3. 按 `Enter` 确认恢复，`Escape` 取消

### 清除当前会话

使用 `/clear` 命令清空当前对话和历史，开始全新对话。

### 会话存储位置

会话文件保存在 `.acecode/projects/<cwd_hash>/` 目录下（基于当前工作目录的哈希值）。最多保留 50 个历史会话（可在配置文件中调整）。

### 上下文压缩

当对话历史较长，接近模型上下文窗口限制时：

- **自动压缩**：acecode 会自动触发压缩，保留摘要以节省 token
- **手动压缩**：随时可以输入 `/compact` 主动压缩当前对话历史

---

## 12. Skills（技能扩展）

Skills 是用户自定义的工作流指令，存储为 Markdown 文件，acecode 启动时自动发现并注册为斜杠命令。

### 技能存放位置

```
~/.acecode/skills/
  <category>/
    <skill-name>/
      SKILL.md          # 必须 — 包含 YAML frontmatter 和指令正文
```

### 使用技能

```
/<skill-name>           # 直接激活技能
/<skill-name> 附加参数   # 激活技能并传入补充说明
```

**示例：**
```
/plan 为用户注册模块设计实现方案
```

### 管理技能

| 命令 | 说明 |
|------|------|
| `/skills` | 列出所有已安装技能（按分类分组） |
| `/skills reload` | 重新扫描磁盘（编辑完 SKILL.md 后使用） |
| `/skills help` | 显示技能系统使用帮助 |

### 创建技能

在 `~/.acecode/skills/<category>/<name>/SKILL.md` 中创建文件：

```markdown
---
name: my-skill
description: 这个技能的简短描述，供 AI 和下拉菜单展示
tags: [tag1, tag2]
---

# My Skill

... 给 AI 的详细指令内容 ...
```

`name` 字段会自动转为 kebab-case 作为斜杠命令名（如 `name: My Plan` → `/my-plan`）。

### 禁用/添加外部技能目录

在 `~/.acecode/config.json` 中：

```json
{
  "skills": {
    "disabled": ["my-skill"],
    "external_dirs": ["~/work/team-skills"]
  }
}
```

---

## 13. MCP 服务器

acecode 支持通过 MCP（Model Context Protocol）接入外部工具服务器，扩展 AI 的工具能力。支持三种传输方式：

- **`stdio`**（默认）：启动本地子进程，经由管道通信
- **`sse`**：旧版 HTTP + `text/event-stream` 双端点协议（`/sse` 收事件 + `/message` 发消息）
- **`http`**：2025-03-26 MCP Streamable HTTP 规范的单端点协议。每个 JSON-RPC 消息通过 POST 发到同一端点（默认 `/mcp`），响应可以是 `application/json` 或 `text/event-stream`；会话 ID 在首次 `initialize` 后通过 `Mcp-Session-Id` 响应头下发，后续每次请求都需要回传

### 配置格式

在 `~/.acecode/config.json` 中添加 `mcp_servers` 字段，每个键是你给服务器起的**名称**，值是该服务器的配置项。通过可选的 `transport` 字段选择传输方式，缺省为 `stdio`：

```json
{
  "mcp_servers": {
    "<stdio 服务器>": {
      "command": "<可执行程序路径>",
      "args": ["<参数1>"],
      "env": { "ENV_VAR": "value" }
    },
    "<远端 SSE 服务器>": {
      "transport": "sse",
      "url": "https://mcp.example.com",
      "sse_endpoint": "/sse",
      "auth_token": "<bearer token>",
      "headers": { "X-Team": "acecode" },
      "timeout_seconds": 30,
      "validate_certificates": true,
      "ca_cert_path": ""
    }
  }
}
```

**stdio 字段：**

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `command` | string | **是** | 要启动的 MCP 服务器可执行程序 |
| `args` | string[] | 否 | 命令行参数列表 |
| `env` | object | 否 | 注入到服务器进程的额外环境变量 |

**sse / http 字段：**

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `url` | string | **是** | 服务器基础 URL（`scheme://host[:port]`） |
| `sse_endpoint` | string | 否 | 端点路径。`sse` 传输默认 `/sse`；`http` 传输默认 `/mcp`（字段名复用，未来若冲突严重可能拆分） |
| `headers` | object | 否 | 额外请求头（敏感值不会出现在日志里） |
| `auth_token` | string | 否 | Bearer 令牌（日志仅输出 `auth: present`） |
| `timeout_seconds` | int | 否 | 请求超时，默认 30 |
| `validate_certificates` | bool | 否 | 是否校验 TLS 证书，默认 `true`；关闭时会在日志中打印 WARNING |
| `ca_cert_path` | string | 否 | 自签证书 CA 路径 |

> 未含 `transport` 字段的旧版条目仍作为 stdio 处理，行为与先前版本完全一致。
>
> 服务器名称会被清理（非字母数字字符替换为 `_`）后作为工具名前缀。例如名称为 `my-tools`，其工具 `search` 会被注册为 `mcp_my_tools_search`。

### 配置示例

**示例一：使用 npx 启动 Node.js MCP 服务器**

```json
{
  "mcp_servers": {
    "filesystem": {
      "command": "npx",
      "args": ["-y", "@modelcontextprotocol/server-filesystem", "/path/to/allowed/dir"]
    }
  }
}
```

**示例二：启动本地可执行文件并传入 API Key**

```json
{
  "mcp_servers": {
    "my-search": {
      "command": "/usr/local/bin/mcp-search-server",
      "args": ["--port", "0"],
      "env": {
        "SEARCH_API_KEY": "sk-xxxx",
        "LOG_LEVEL": "warn"
      }
    }
  }
}
```

**示例三：同时配置多个 MCP 服务器（混合 stdio 与远端）**

```json
{
  "mcp_servers": {
    "filesystem": {
      "command": "npx",
      "args": ["-y", "@modelcontextprotocol/server-filesystem", "~/projects"]
    },
    "github": {
      "command": "npx",
      "args": ["-y", "@modelcontextprotocol/server-github"],
      "env": {
        "GITHUB_PERSONAL_ACCESS_TOKEN": "ghp_xxxx"
      }
    },
    "remote-sse": {
      "transport": "sse",
      "url": "https://mcp.internal.example.com",
      "sse_endpoint": "/sse",
      "auth_token": "sk-internal-xxxx",
      "headers": { "X-Project": "acecode" }
    },
    "remote-http": {
      "transport": "http",
      "url": "https://mcp-streamable.example.com"
      // sse_endpoint 省略时对 http 传输默认为 "/mcp"
    }
  }
}
```

### 工作原理

acecode 启动时按 transport 连接每个配置的 MCP 服务器：

1. **stdio**：启动子进程并注入 `env` 环境变量，通过管道完成 MCP 握手
2. **sse**：建立到 `url + sse_endpoint`（默认 `/sse`）的 SSE 流，从 `endpoint` 事件拿到消息端点，完成 MCP 握手
3. **http（Streamable HTTP）**：POST `initialize` 到 `url + sse_endpoint`（默认 `/mcp`），从响应头读取 `Mcp-Session-Id`，后续每个 JSON-RPC 消息都带上该 session 头
4. 获取服务器暴露的工具列表，注册为 `mcp_<服务器名>_<工具名>` 形式
5. AI 可以像使用内置工具一样调用这些 MCP 工具

单个服务器连接失败不会影响其他服务器，会记录日志并跳过。TLS 校验关闭（`validate_certificates: false`）时会在日志中输出醒目 WARNING。

### 启动时状态显示

如有配置的 MCP 服务器，启动后对话区会显示连接情况：

```
[MCP] Connected 2 server(s), registered 8 external tool(s).
```

若某个服务器连接失败，可在当前工作目录的 `acecode.log` 中查看详细错误信息。

### 管理 MCP 连接

启动后可以用以下子命令管理 MCP 服务器：

| 命令 | 说明 |
|------|------|
| `/mcp` | 列出所有服务器、状态（connected/disabled/failed）、transport 类型和工具数 |
| `/mcp list` | 列出所有 MCP 工具，按服务器分组，含工具名和描述 |
| `/mcp enable <name>` | 重新连接某个 disabled 或 failed 的服务器 |
| `/mcp disable <name>` | 停止某个服务器的子进程/连接，并从工具注册表中注销其工具 |
| `/mcp reconnect <name>` | 强制断开后重新连接（配置更新或进程崩溃后使用） |
| `/mcp help` | 显示子命令帮助 |

---

## 14. 配置文件详解

配置文件路径：`~/.acecode/config.json`

```json
{
  "provider": "copilot",
  "openai": {
    "base_url": "http://localhost:1234/v1",
    "api_key": "your-api-key",
    "model": "your-model-name"
  },
  "copilot": {
    "model": "gpt-4o"
  },
  "context_window": 128000,
  "max_sessions": 50,
  "skills": {
    "disabled": [],
    "external_dirs": []
  }
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `provider` | string | `"copilot"` 或 `"openai"` |
| `openai.base_url` | string | OpenAI 兼容 API 的地址 |
| `openai.api_key` | string | API 鉴权密钥 |
| `openai.model` | string | 使用的模型名称 |
| `copilot.model` | string | Copilot 模型名（默认 `gpt-4o`） |
| `context_window` | number | 上下文窗口大小（token 数），通常自动解析 |
| `max_sessions` | number | 最多保留的历史会话数（默认 50） |
| `skills.disabled` | array | 禁用的技能名称列表 |
| `skills.external_dirs` | array | 额外的技能扫描目录 |

运行 `./acecode configure` 可以通过交互式向导修改配置。

---

## 15. 常见问题

### Q: 启动后显示 "Authenticating with GitHub Copilot..."

这是正常的首次认证流程。按照界面提示，在浏览器中打开链接并输入验证码即可。认证完成后 token 会自动保存，以后启动直接使用。

### Q: AI 正在回复，我能中断它吗？

可以。按 `Escape` 键即可中止当前的 AI 响应或工具执行。

### Q: 如何退出 acecode？

输入 `/exit` 命令，或在输入框为空的状态下连续按两次 `Ctrl+C`（间隔在 1.2 秒以内）。

### Q: 对话历史太长导致 token 超限怎么办？

- acecode 会自动在接近上下文限制时触发压缩
- 也可以主动输入 `/compact` 手动压缩
- 若要完全清空历史重新开始，使用 `/clear`

### Q: 如何切换模型？

使用 `/model <模型名>` 命令，例如：
```
/model claude-3.5-sonnet
/model gpt-4o
```

### Q: 工具确认弹窗总是出现，能否减少干扰？

- 按 `a`（always）可以让该工具在本次会话内不再确认
- 按 `Ctrl+P` 切换到 **AcceptEdits** 模式，文件操作会自动通过
- 若完全不想要确认，切换到 **Yolo** 模式（或启动时加 `-dangerous`）

### Q: 日志文件在哪里？

acecode 运行时会在当前工作目录生成 `acecode.log` 文件，记录详细的调试信息。
