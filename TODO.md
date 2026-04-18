# AceCode 改进待办清单

## 🔴 高优先级（立即处理）

### 代码重构
- [ ] 完成bash_tool、glob_tool、grep_tool的重构以使用新工具类
- [ ] 提取公共的fs_scan_utils模块用于glob和grep
- [ ] 拆分main.cpp到多个模块：
  - [ ] cli_parser.cpp - 命令行参数解析
  - [ ] application.cpp - 应用生命周期
  - [ ] chat_ui.cpp - TUI界面
  - [ ] thinking_animation.cpp - 动画逻辑

### 并发模型改进
- [ ] 移除所有detach()线程，使用单worker线程模型
- [ ] 为AgentLoop添加内部任务队列确保串行执行
- [ ] 统一退出路径，移除重复的finalize调用
- [ ] 明确定义跨线程回调的规则（只允许PostEvent）

### 权限系统
- [ ] 将权限决策从bool改为三态：Allow/Prompt/Deny
- [ ] 修复Deny规则仍可能弹确认框的bug
- [ ] 完善PathValidator的路径验证逻辑
- [ ] 为bash工具添加更严格的默认deny规则

## 🟡 中优先级（本周完成）

### 文档和配置
- [ ] 确保README.md和README_CN.md存在（CI打包需要）
- [ ] 添加开发环境搭建文档
- [ ] 添加权限系统说明文档
- [ ] 添加配置文件详细说明

### 代码质量
- [ ] 替换所有magic number为constants.hpp中的常量
- [ ] 改进TuiState接口添加辅助方法
- [ ] 统一版本号管理（避免hardcode）
- [ ] 为config.cpp添加配置校验
- [ ] 改进Windows IME代码，使用std::max/std::clamp替代自定义函数

### 测试
- [ ] 添加ctest支持到CMakeLists.txt
- [ ] 为新的工具类添加单元测试
- [ ] 为FileOperations添加边界测试
- [ ] 为ToolArgsParser添加测试

## 🟢 低优先级（持续优化）

### 平台特定
- [ ] 分离Windows特定代码到platform/windows/
- [ ] 分离POSIX特定代码到platform/posix/
- [ ] 统一跨平台的信号处理

### CI/CD
- [ ] 添加lint/format检查到GitHub Actions
- [ ] 添加AddressSanitizer/UBSan job
- [ ] 添加测试阶段到CI流程
- [ ] 考虑添加clang-tidy静态检查

### 用户体验
- [ ] 添加--help参数支持
- [ ] 添加--version参数
- [ ] 添加--debug参数控制日志级别
- [ ] 默认日志级别改为Info（当前是Dbg）
- [ ] 改进错误消息对用户更友好
- [ ] 增强会话管理：/sessions命令、删除会话等

### 代码组织
- [ ] 考虑CMake按模块显式列源文件（替代GLOB_RECURSE）
- [ ] 思考短语移到单独的配置类
- [ ] 统一日志记录宏
- [ ] 提取命令处理的重复模式

## 📋 已完成

- [x] 创建ToolArgsParser统一JSON参数解析
- [x] 创建ToolErrors统一错误消息
- [x] 创建FileOperations统一文件操作
- [x] 创建constants.hpp集中管理常量
- [x] 重构file_read_tool.cpp
- [x] 重构file_write_tool.cpp
- [x] 重构file_edit_tool.cpp
- [x] 编写REFACTORING_SUMMARY.md文档

## 🐛 已知问题

1. **main.cpp过于庞大** - 1000+行，职责混杂
2. **并发模型偏脆** - detach线程，生命周期管理风险
3. **权限语义有歧义** - Deny和Prompt未明确区分
4. **版本号不一致** - CMakeLists.txt和代码中硬编码的版本号不同
5. **grep的include_pattern功能弱** - 不支持{cpp,hpp}语法
6. **glob和grep有重复逻辑** - 应该共享目录扫描代码
7. **.gitignore过于激进** - 忽略了.github和openspec目录

## 💡 架构改进建议

### 建议的并发模型
```
UI Thread ─────────────> Agent Worker Thread
    │                           │
    ├─ 渲染                     ├─ submit()
    ├─ 输入                     ├─ tool执行
    ├─ 接收events               ├─ provider调用
    │                           └─ 发送events
    └─ PostEvent() <──────────────┘
```

### 建议的目录结构
```
src/
├── app/              # 应用层
│   ├── main.cpp
│   ├── application.cpp
│   └── cli_parser.cpp
├── tui/              # UI层
│   ├── chat_ui.cpp
│   └── thinking_animation.cpp
├── agent/            # Agent核心
│   ├── agent_loop.cpp
│   └── tool_executor.cpp
├── platform/         # 平台特定
│   ├── windows/
│   └── posix/
└── utils/            # 通用工具
    ├── tool_args_parser.hpp
    ├── tool_errors.hpp
    ├── file_operations.hpp
    └── constants.hpp
```

## 📊 重构进度

- 工具类创建: **100%** (4/4)
- 文件工具重构: **100%** (3/3)
- 其他工具重构: **0%** (0/3)
- main.cpp分离: **0%**
- 并发模型改进: **0%**
- 权限系统改进: **0%**
- 测试覆盖: **0%**

**总体进度: ~15%**

---

_最后更新: 2025-01-XX_
_维护者: @shaohaozhi286_
