# AceCode 架构文档

## 总体架构

```
┌─────────────────────────────────────────────────────────────┐
│                        User (Terminal)                       │
└────────────────────────┬────────────────────────────────────┘
                         │
┌────────────────────────▼────────────────────────────────────┐
│                     TUI Layer (FTXUI)                        │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐     │
│  │ Chat View    │  │ Input Box    │  │ Status Bar   │     │
│  └──────────────┘  └──────────────┘  └──────────────┘     │
│  ┌──────────────┐  ┌──────────────┐                        │
│  │ MD Renderer  │  │ Thinking     │                        │
│  └──────────────┘  └──────────────┘                        │
└────────────────────────┬────────────────────────────────────┘
                         │
┌────────────────────────▼────────────────────────────────────┐
│                    Application Layer                         │
│  ┌──────────────────────────────────────────────────────┐  │
│  │              Agent Loop (submit/cancel)              │  │
│  └───────┬──────────────────────────────────────┬───────┘  │
│          │                                       │          │
│  ┌───────▼───────┐                     ┌────────▼────────┐ │
│  │   Provider    │                     │  Tool Executor  │ │
│  │ ┌───────────┐ │                     │ ┌─────────────┐ │ │
│  │ │  OpenAI   │ │                     │ │   Bash      │ │ │
│  │ │  Copilot  │ │                     │ │   FileRead  │ │ │
│  │ └───────────┘ │                     │ │   FileWrite │ │ │
│  └───────────────┘                     │ │   FileEdit  │ │ │
│                                         │ │   Grep      │ │ │
│  ┌───────────────┐                     │ │   Glob      │ │ │
│  │   Session     │                     │ └─────────────┘ │ │
│  │   Manager     │                     └─────────────────┘ │
│  └───────────────┘                     ┌─────────────────┐ │
│                                         │  Permission     │ │
│  ┌───────────────┐                     │  Manager        │ │
│  │   Config      │                     └─────────────────┘ │
│  └───────────────┘                                          │
└─────────────────────────────────────────────────────────────┘
```

## 模块说明

### 1. TUI Layer（用户界面层）

**职责**：处理所有用户交互和显示

**核心组件**：
- `TuiState` - 界面状态管理
- `ChatView` - 对话显示（支持markdown）
- `InputBox` - 用户输入（带历史记录）
- `StatusBar` - 状态显示
- `ThinkingAnimation` - 思考动画

**依赖**：FTXUI库

**线程**：运行在UI主线程

### 2. Application Layer（应用层）

#### AgentLoop
**职责**：核心业务逻辑，协调provider和tool

**主要方法**：
- `submit(prompt)` - 提交用户请求
- `cancel()` - 取消当前执行
- `resume(session_id)` - 恢复会话

**流程**：
```
submit() 
  → 添加user消息
  → 调用provider.chat_completion() 
  → 处理assistant response
  → 如果有tool_calls:
      → 逐个执行tool (需要权限确认)
      → 添加tool_result消息
      → 递归调用provider (续写)
  → 完成
```

**当前问题**：
- ⚠️ 内部无串行化保证，依赖外部调用约定
- ⚠️ detach线程生命周期管理风险

#### Provider（LLM提供者）
**接口**：
```cpp
ChatCompletionResult chat_completion(
    const std::vector<ChatMessage>& messages,
    const std::vector<ToolDef>& tools,
    StreamCallback on_delta,
    std::atomic<bool>& abort_requested
);
```

**实现**：
- `OpenAIProvider` - OpenAI兼容API
- `CopilotProvider` - GitHub Copilot

**特性**：
- 流式输出支持
- 取消支持
- 自动token刷新（Copilot）

#### Tool Executor
**职责**：执行工具调用并返回结果

**工具列表**：
| 工具 | 只读 | 说明 |
|------|------|------|
| bash | ❌ | 执行shell命令 |
| file_read | ✅ | 读取文件 |
| file_write | ❌ | 写入文件 |
| file_edit | ❌ | 编辑文件 |
| grep | ✅ | 正则搜索 |
| glob | ✅ | 文件查找 |

**权限流程**：
```
execute(tool_name, args)
  → PathValidator.validate(path) - 路径验证
  → PermissionManager.should_auto_allow() - 权限决策
  → 如果需要确认:
      → on_tool_confirm() 回调UI
      → 等待用户决策
  → 执行工具
  → 返回ToolResult
```

**当前问题**：
- ⚠️ Deny和Prompt语义混淆
- ⚠️ bash工具安全规则不足

#### Session Manager
**职责**：会话持久化

**功能**：
- 保存对话历史到磁盘
- 按项目CWD分类存储
- 恢复历史会话
- 自动清理旧会话

**存储结构**：
```
~/.acecode/
  ├── config.json
  ├── copilot_token.json
  └── projects/
      └── <cwd_hash>/
          ├── session_<timestamp>.json
          └── ...
```

#### Config Manager
**职责**：配置管理

**配置项**：
- provider选择
- API endpoint/key
- model名称
- 会话保留策略

### 3. Utils Layer（工具层）

**新增工具类**（✨本次重构）：
- `ToolArgsParser` - JSON参数解析
- `ToolErrors` - 错误消息标准化
- `FileOperations` - 文件操作封装
- `constants` - 常量定义

**原有工具**：
- `Logger` - 日志
- `PathValidator` - 路径验证
- `encoding` - UTF-8转换
- `uuid` - UUID生成
- `diff_utils` - 差异生成

## 数据流

### 1. 用户提交请求
```
User Input
  ↓
TUI (Event)
  ↓
main.cpp (detach thread) ⚠️
  ↓
AgentLoop.submit()
  ↓
Provider.chat_completion()
  ↓ (streaming)
Callback → TUI update
```

### 2. 工具调用
```
Provider返回tool_calls
  ↓
AgentLoop遍历tool_calls
  ↓
ToolExecutor.execute()
  ↓
PathValidator.validate()
  ↓
PermissionManager.check()
  ↓ (如需确认)
UI回调 → 用户选择
  ↓
Tool实现 (bash/file_read/...)
  ↓
返回ToolResult
  ↓
添加tool_result消息
  ↓
继续调用Provider (带tool_result)
```

## 并发模型

### 当前模型（有问题⚠️）

```
UI Thread
  ├─ FTXUI事件循环
  ├─ 渲染
  └─ 收集输入
      ↓
      创建detach线程执行submit() ⚠️
      
Detached Thread (每次submit一个) ⚠️
  ├─ AgentLoop.submit()
  ├─ 调用Provider
  ├─ 执行Tools
  └─ 回调UI (PostEvent)
  
Animation Thread (thinking)
  └─ 定时更新动画状态 → PostEvent
```

**问题**：
- detach线程在程序退出时可能悬空
- 多个线程PostEvent，顺序不确定
- 状态访问需要频繁加锁
- 取消机制不完善

### 建议模型（TODO）

```
UI Thread
  ├─ FTXUI事件循环
  ├─ 渲染
  ├─ 处理events (来自worker)
  └─ 发送任务到worker队列
  
Worker Thread (单例)
  ├─ 从队列取任务
  ├─ 执行submit/cancel/resume
  ├─ 所有AgentLoop状态访问在此线程
  └─ 通过PostEvent通知UI
  
  所有状态修改串行化 ✅
  join即可安全退出 ✅
```

## 配置与扩展点

### 添加新工具

1. 创建 `src/tool/my_tool.cpp`:
```cpp
#include "utils/tool_args_parser.hpp"
#include "utils/tool_errors.hpp"

static ToolResult execute_my_tool(const std::string& args_json) {
    ToolArgsParser parser(args_json);
    if (parser.has_error()) {
        return ToolResult{parser.error(), false};
    }
    
    std::string param = parser.get_or<std::string>("param", "");
    if (param.empty()) {
        return ToolResult{ToolErrors::missing_parameter("param"), false};
    }
    
    // 实现逻辑
    return ToolResult{result, true};
}

ToolImpl create_my_tool() {
    // 定义schema
    ToolDef def;
    def.name = "my_tool";
    def.description = "...";
    def.parameters = {...};
    
    return ToolImpl{def, execute_my_tool, /*is_read_only=*/true};
}
```

2. 在 `tool_executor.cpp` 中注册:
```cpp
tools_.push_back(create_my_tool());
```

### 添加新Provider

1. 继承 `Provider` 接口
2. 实现 `chat_completion()` 方法
3. 在 `main.cpp` 中根据配置创建实例

### 添加新命令

1. 在 `builtin_commands.cpp` 中添加处理函数:
```cpp
static void cmd_my_command(CommandContext& ctx, const std::string& args) {
    // 实现
}
```

2. 注册到 `execute_builtin_command()`:
```cpp
commands["/mycommand"] = cmd_my_command;
```

## 安全模型

### 路径验证
- 所有文件操作路径必须在CWD内
- 使用 `std::filesystem::weakly_canonical` 规范化
- 前缀匹配检查

### 权限管理
- 只读工具默认auto-allow
- 写/执行工具默认prompt
- 支持自定义规则（allow/deny）
- `--dangerous` 模式跳过所有确认⚠️

### Mtime冲突检测
- file_read时记录mtime
- file_write/edit前检查mtime
- 如果外部修改则拒绝写入

## 性能考虑

### Token管理
- `/compact` 命令压缩历史
- 自动检测PTL错误并重试
- 估算：~4 chars per token

### 输出限制
- bash输出：100KB截断
- file_read：10MB限制
- grep结果：200条限制
- glob结果：500条限制

### 缓存
- Session按需加载
- Copilot token缓存到磁盘
- Mtime记录在内存

## 测试策略（TODO）

### 单元测试
- [ ] ToolArgsParser
- [ ] ToolErrors
- [ ] FileOperations
- [ ] PathValidator
- [ ] PermissionManager

### 集成测试
- [ ] AgentLoop基本流程
- [ ] Tool执行
- [ ] Session保存/恢复

### E2E测试
- [ ] 完整对话流程
- [ ] 工具调用确认
- [ ] 取消机制

## 依赖关系

```
acecode
├── ftxui (TUI framework)
├── nlohmann-json (JSON解析)
├── cpr (HTTP client)
│   └── libcurl (>= 8.14 for Windows TLS)
└── C++17 标准库
```

## 部署

### 编译要求
- CMake >= 3.20
- C++17 compiler
- vcpkg

### 运行要求
- 交互式TTY
- 网络连接（调用LLM API）
- 对项目目录的读写权限

---

_架构版本: v0.1 (重构进行中)_  
_最后更新: 2025-01-XX_
