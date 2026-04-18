# AceCode 开发者快速参考

## 🚀 快速开始

### 首次设置
```bash
# 克隆项目
git clone --recursive https://github.com/shaohaozhi286/acecode.git
cd acecode

# 安装依赖（需要vcpkg）
vcpkg install cpr nlohmann-json ftxui --triplet x64-windows --overlay-ports=./ports

# 配置和编译
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release

# 运行
./build/acecode configure
./build/acecode
```

## 📁 项目结构

```
acecode/
├── src/
│   ├── app/              # 应用层 (TODO: 待创建)
│   ├── tui/              # UI层 (目前在main.cpp)
│   ├── agent/            # Agent逻辑
│   │   └── agent_loop.*
│   ├── provider/         # LLM提供者
│   │   ├── openai_provider.*
│   │   └── copilot_provider.*
│   ├── tool/             # 工具实现
│   │   ├── bash_tool.*
│   │   ├── file_*_tool.*
│   │   ├── grep_tool.*
│   │   └── glob_tool.*
│   ├── session/          # 会话管理
│   ├── config/           # 配置管理
│   ├── commands/         # 内置命令
│   ├── utils/            # 通用工具 ⭐
│   │   ├── tool_args_parser.hpp    # 新增
│   │   ├── tool_errors.hpp         # 新增
│   │   ├── file_operations.hpp     # 新增
│   │   └── constants.hpp           # 新增
│   └── main.cpp          # 入口 (⚠️需要拆分)
├── scripts/              # 工具脚本
│   ├── code_quality_check.bat
│   └── code_quality_check.sh
├── docs/                 # 文档
│   └── (未来添加详细文档)
├── ARCHITECTURE.md       # 架构文档
├── TODO.md              # 待办清单
├── REFACTORING_SUMMARY.md
└── README.md            # 用户文档
```

## 🛠️ 常用命令

### 编译
```bash
# 完整重新编译
cmake --build build --config Release --clean-first

# 增量编译
cmake --build build --config Release

# 只编译不链接（检查语法）
cmake --build build --config Release --target acecode -- -n
```

### 代码质量检查
```bash
# Windows
scripts\code_quality_check.bat

# Linux/Mac
./scripts/code_quality_check.sh
```

### 清理
```bash
# 清理构建产物
rm -rf build

# 清理会话数据
rm -rf ~/.acecode/projects
```

## 📝 开发工作流

### 添加新工具
1. 创建 `src/tool/my_tool.cpp`
2. 使用 `ToolArgsParser` 解析参数
3. 使用 `ToolErrors` 返回错误
4. 在 `tool_executor.cpp` 注册
5. 更新文档

### 修改现有代码
1. 运行代码质量检查
2. 进行修改
3. 编译测试
4. 再次运行质量检查
5. 更新TODO.md
6. 提交

### 重构流程
1. 在TODO.md标记任务
2. 创建分支（可选）
3. 重构
4. 测试
5. 更新REFACTORING_SUMMARY.md
6. 提交

## 🔧 常见工具类用法

### ToolArgsParser
```cpp
#include "utils/tool_args_parser.hpp"

ToolArgsParser parser(arguments_json);
if (parser.has_error()) {
    return ToolResult{parser.error(), false};
}

std::string path = parser.get_or<std::string>("file_path", "");
int timeout = parser.get_or<int>("timeout_ms", 120000);
```

### ToolErrors
```cpp
#include "utils/tool_errors.hpp"

// 简单错误
return ToolResult{ToolErrors::missing_parameter("file_path"), false};

// 文件未找到
return ToolResult{ToolErrors::file_not_found(path, cwd), false};

// 外部修改
return ToolResult{ToolErrors::external_modification(path), false};
```

### FileOperations
```cpp
#include "utils/file_operations.hpp"

// 检查文件存在
auto check = FileOperations::check_file_exists(path);
if (!check.success) return check;

// 检查文件大小
auto size_check = FileOperations::check_file_size(path, "提示信息");
if (!size_check.success) return size_check;

// 读取内容
std::string content, error;
if (!FileOperations::read_content(path, content, error)) {
    return ToolResult{error, false};
}

// 写入内容
if (!FileOperations::write_content(path, content, error)) {
    return ToolResult{error, false};
}
```

### Constants
```cpp
#include "utils/constants.hpp"

using namespace acecode::constants;

// 使用常量
if (file_size > MAX_FILE_SIZE) { ... }
int tokens = chars / CHARS_PER_TOKEN;
```

## 🐛 调试技巧

### 启用调试日志
```cpp
// main.cpp
Logger::instance().set_level(LogLevel::Dbg);
```

### 查看会话文件
```bash
# 找到项目会话目录
cat ~/.acecode/projects/*/session_*.json
```

### 检查配置
```bash
cat ~/.acecode/config.json
```

### Windows调试
```bash
# 查看详细编译信息
cmake --build build --config Release --verbose
```

## 📊 代码规范

### 命名约定
- 文件：`snake_case.cpp`
- 类：`PascalCase`
- 函数/变量：`snake_case`
- 常量：`UPPER_SNAKE_CASE`
- 成员变量：`snake_case_`（末尾下划线）

### 代码组织
- 头文件包含顺序：
  1. 对应.hpp
  2. 项目头文件
  3. 第三方库
  4. 标准库
- 命名空间：`namespace acecode { ... }`
- 缩进：4空格
- 行宽：建议不超过100字符

### 错误处理
- 工具函数返回 `ToolResult{content, success}`
- 使用 `ToolErrors` 类生成错误消息
- 避免硬编码错误字符串
- 日志使用 `LOG_DEBUG/INFO/WARN/ERROR`

### 注释
- 公共API需要注释
- 复杂逻辑需要解释
- 避免无意义的注释
- TODO标记待处理问题

## 🔍 常见问题

### 编译失败
```bash
# 确保vcpkg依赖正确
vcpkg list

# 重新配置CMake
rm -rf build
cmake -B build ...

# 检查编译器版本
cmake --version
g++ --version  # 或 cl.exe
```

### 工具不执行
1. 检查权限规则
2. 查看日志输出
3. 确认路径在CWD内
4. 使用 `--dangerous` 跳过确认测试

### 会话无法恢复
1. 检查 `~/.acecode/projects/` 目录
2. 查看session文件格式
3. 尝试从备份恢复

## 📚 参考文档

- [架构文档](ARCHITECTURE.md) - 系统架构和设计
- [待办清单](TODO.md) - 开发任务
- [重构总结](REFACTORING_SUMMARY.md) - 重构进度
- [项目整理](PROJECT_ORGANIZATION.md) - 整理成果
- [用户手册](README.md) - 用户使用指南

## 🎯 重构优先级

当前重点：
1. ✅ 工具类重构（已完成3/6）
2. ⬜ 继续重构bash/glob/grep
3. ⬜ 提取fs_scan_utils公共模块
4. ⬜ 拆分main.cpp
5. ⬜ 改进并发模型

详见 [TODO.md](TODO.md)

## 💡 最佳实践

### 添加功能前
1. 检查是否有可复用的工具类
2. 查看类似功能的实现
3. 遵循现有代码风格

### 重构时
1. 一次只改一个模块
2. 确保编译通过
3. 手动测试核心流程
4. 更新相关文档

### 提交前
1. 运行质量检查脚本
2. 确保无编译警告
3. 检查TODO.md是否需要更新
4. 写清晰的提交消息

---

**快速链接**
- [GitHub Repo](https://github.com/shaohaozhi286/acecode)
- [Issues](https://github.com/shaohaozhi286/acecode/issues)
- [Releases](https://github.com/shaohaozhi286/acecode/releases)

**获取帮助**
- 查看文档目录
- 搜索代码中的注释
- 运行 `./acecode --help`

---

_快速参考 v1.0_  
_最后更新: 2025-01-XX_
