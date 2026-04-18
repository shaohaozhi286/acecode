# AceCode 文档索引

欢迎查阅AceCode项目文档！本文档索引帮助你快速找到所需信息。

## 📖 文档分类

### 🚀 新手入门
- **[README.md](README.md)** - 项目介绍、快速开始、功能说明（用户向）
- **[README_CN.md](README_CN.md)** - 中文版README
- **[QUICK_REFERENCE.md](QUICK_REFERENCE.md)** - 开发者快速参考手册

### 🏗️ 架构与设计
- **[ARCHITECTURE.md](ARCHITECTURE.md)** - 系统架构、模块说明、数据流
- **[PROJECT_ORGANIZATION.md](PROJECT_ORGANIZATION.md)** - 项目整理成果总结

### 📋 任务与进度
- **[TODO.md](TODO.md)** - 详细待办清单（按优先级）
- **[REFACTORING_SUMMARY.md](REFACTORING_SUMMARY.md)** - 重构进度跟踪
- **[REFACTORING_COMPLETE.md](REFACTORING_COMPLETE.md)** - 第一阶段整理报告

### 🛠️ 开发工具
- **[scripts/code_quality_check.bat](scripts/code_quality_check.bat)** - Windows代码质量检查
- **[scripts/code_quality_check.sh](scripts/code_quality_check.sh)** - Linux/Mac代码质量检查
- **[.editorconfig](.editorconfig)** - 编辑器配置

---

## 🎯 按场景查找

### 我想了解项目是什么
→ [README.md](README.md)

### 我想开始开发
→ [QUICK_REFERENCE.md](QUICK_REFERENCE.md) → [ARCHITECTURE.md](ARCHITECTURE.md)

### 我想了解代码结构
→ [ARCHITECTURE.md](ARCHITECTURE.md) → [QUICK_REFERENCE.md](QUICK_REFERENCE.md#-项目结构)

### 我想查看待办任务
→ [TODO.md](TODO.md)

### 我想了解重构进度
→ [REFACTORING_SUMMARY.md](REFACTORING_SUMMARY.md) → [REFACTORING_COMPLETE.md](REFACTORING_COMPLETE.md)

### 我想添加新功能
→ [QUICK_REFERENCE.md](QUICK_REFERENCE.md#-常用命令) → [ARCHITECTURE.md](ARCHITECTURE.md#配置与扩展点)

### 我想检查代码质量
→ 运行 `scripts/code_quality_check.bat` 或 `.sh`

### 我想理解工具类用法
→ [QUICK_REFERENCE.md](QUICK_REFERENCE.md#-常见工具类用法)

---

## 📚 按角色推荐

### 👤 用户
1. [README.md](README.md) - 了解功能和使用方法
2. [README_CN.md](README_CN.md) - 中文文档

### 👨‍💻 新入职开发者
1. [README.md](README.md) - 了解项目
2. [QUICK_REFERENCE.md](QUICK_REFERENCE.md) - 快速上手
3. [ARCHITECTURE.md](ARCHITECTURE.md) - 理解架构
4. [TODO.md](TODO.md) - 查看任务

### 🔧 核心开发者
1. [ARCHITECTURE.md](ARCHITECTURE.md) - 深入理解设计
2. [TODO.md](TODO.md) - 计划工作
3. [REFACTORING_SUMMARY.md](REFACTORING_SUMMARY.md) - 跟踪进度
4. [QUICK_REFERENCE.md](QUICK_REFERENCE.md) - 日常参考

### 📊 项目管理者
1. [PROJECT_ORGANIZATION.md](PROJECT_ORGANIZATION.md) - 整体状况
2. [TODO.md](TODO.md) - 任务清单
3. [REFACTORING_COMPLETE.md](REFACTORING_COMPLETE.md) - 阶段报告

---

## 🔍 按内容查找

### 功能说明
- 用户功能 → [README.md](README.md#features)
- Slash命令 → [README.md](README.md#slash-commands-in-the-tui)
- 内置工具 → [README.md](README.md#built-in-tools)

### 使用指南
- 快速开始 → [README.md](README.md#quick-start)
- 命令行参数 → [README.md](README.md#command-line-flags)
- 配置说明 → [README.md](README.md#configuration)

### 开发指南
- 编译构建 → [README.md](README.md#how-to-build) + [QUICK_REFERENCE.md](QUICK_REFERENCE.md#-快速开始)
- 添加工具 → [ARCHITECTURE.md](ARCHITECTURE.md#添加新工具)
- 代码规范 → [QUICK_REFERENCE.md](QUICK_REFERENCE.md#-代码规范)
- 工具类使用 → [QUICK_REFERENCE.md](QUICK_REFERENCE.md#-常见工具类用法)

### 架构设计
- 总体架构 → [ARCHITECTURE.md](ARCHITECTURE.md#总体架构)
- 模块说明 → [ARCHITECTURE.md](ARCHITECTURE.md#模块说明)
- 数据流 → [ARCHITECTURE.md](ARCHITECTURE.md#数据流)
- 并发模型 → [ARCHITECTURE.md](ARCHITECTURE.md#并发模型)

### 任务规划
- 待办清单 → [TODO.md](TODO.md)
- 已知问题 → [TODO.md](TODO.md#-已知问题)
- 重构进度 → [REFACTORING_SUMMARY.md](REFACTORING_SUMMARY.md)
- 优先级 → [TODO.md](TODO.md#-高优先级立即处理)

### 质量保障
- 代码质量检查 → `scripts/code_quality_check.*`
- DRY违规 → [REFACTORING_COMPLETE.md](REFACTORING_COMPLETE.md#-改进效果量化)
- 最佳实践 → [REFACTORING_COMPLETE.md](REFACTORING_COMPLETE.md#-最佳实践总结)

---

## 📝 文档维护

### 新增功能时
- 更新 [README.md](README.md) 用户功能说明
- 更新 [ARCHITECTURE.md](ARCHITECTURE.md) 如有架构变更
- 更新 [QUICK_REFERENCE.md](QUICK_REFERENCE.md) 如有新API

### 完成任务时
- 在 [TODO.md](TODO.md) 中标记完成
- 更新 [REFACTORING_SUMMARY.md](REFACTORING_SUMMARY.md) 进度

### 发现问题时
- 添加到 [TODO.md](TODO.md) 已知问题

### 重构完成时
- 更新 [REFACTORING_SUMMARY.md](REFACTORING_SUMMARY.md)
- 考虑创建新的阶段报告

---

## 🔗 快速链接

### 外部资源
- [GitHub仓库](https://github.com/shaohaozhi286/acecode)
- [Issue追踪](https://github.com/shaohaozhi286/acecode/issues)
- [发布页面](https://github.com/shaohaozhi286/acecode/releases)

### 依赖文档
- [FTXUI](https://github.com/ArthurSonzogni/FTXUI)
- [nlohmann-json](https://github.com/nlohmann/json)
- [cpr](https://github.com/libcpr/cpr)

---

## 📊 文档统计

| 文档类型 | 数量 | 总字数 |
|----------|------|--------|
| 用户文档 | 2个 | ~5000字 |
| 开发文档 | 5个 | ~13000字 |
| 工具脚本 | 2个 | - |
| 配置文件 | 1个 | - |
| **总计** | **10个** | **~18000字** |

---

## 💡 使用建议

1. **首次接触项目** - 按顺序阅读：README → QUICK_REFERENCE → ARCHITECTURE
2. **日常开发** - 主要参考：QUICK_REFERENCE + TODO
3. **深入研究** - 仔细阅读：ARCHITECTURE + REFACTORING_SUMMARY
4. **定期检查** - 运行质量检查脚本，查看TODO进展

---

## 📞 获取帮助

- 查不到内容？→ 使用Ctrl+F全局搜索关键词
- 文档有错？→ 提交Issue或PR
- 需要新文档？→ 在TODO.md中提出需求

---

_文档索引 v1.0_  
_最后更新: 2025-01-XX_  
_维护者: AceCode Team_

**🎯 温馨提示**: 收藏本文档，作为项目文档导航入口！
