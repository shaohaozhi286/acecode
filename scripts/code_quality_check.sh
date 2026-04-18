#!/bin/bash
# AceCode - 代码整理检查脚本
# 用于检测常见的DRY违规和代码质量问题

set -e

echo "========================================"
echo "AceCode 代码质量检查"
echo "========================================"
echo ""

echo "[1/6] 检查重复的JSON解析模式..."
if grep -r "json::parse(arguments_json)" src/*.cpp 2>/dev/null | grep -v ToolArgsParser; then
    echo "⚠ 发现未使用ToolArgsParser的文件 (见上方)"
else
    echo "✓ 所有工具都使用了ToolArgsParser"
fi
echo ""

echo "[2/6] 检查硬编码的错误消息..."
if grep -r '\[Error\]' src/tool/*.cpp 2>/dev/null | grep -v "ToolErrors::"; then
    echo "⚠ 发现硬编码的错误消息 (见上方)"
else
    echo "✓ 所有错误消息都使用了ToolErrors类"
fi
echo ""

echo "[3/6] 检查magic numbers..."
echo "⚠ 搜索常见的magic numbers:"
grep -rn "/ 4" src/*.cpp 2>/dev/null | grep -v "constants::" | grep -v "//" || echo "  (无)"
grep -rn "10 \* 1024 \* 1024" src/*.cpp 2>/dev/null | grep -v "constants::" || echo "  (无)"
grep -rn "100 \* 1024" src/*.cpp 2>/dev/null | grep -v "constants::" || echo "  (无)"
echo ""

echo "[4/6] 检查TODO/FIXME注释..."
if grep -rn "TODO\|FIXME\|XXX" src/*.cpp src/*.hpp 2>/dev/null; then
    echo "(发现TODO/FIXME注释)"
else
    echo "✓ 未发现TODO/FIXME注释"
fi
echo ""

echo "[5/6] 检查过长的文件..."
echo "文件行数排行 (前10):"
find src -name "*.cpp" -o -name "*.hpp" | xargs wc -l | sort -rn | head -11
echo ""

echo "[6/6] 统计代码行数..."
CPP_COUNT=$(find src -name "*.cpp" | wc -l)
HPP_COUNT=$(find src -name "*.hpp" | wc -l)
TOTAL_LINES=$(find src -name "*.cpp" -o -name "*.hpp" | xargs cat | wc -l)
echo "  - .cpp文件: $CPP_COUNT 个"
echo "  - .hpp文件: $HPP_COUNT 个"
echo "  - 总行数: $TOTAL_LINES 行"
echo ""

echo "========================================"
echo "检查完成"
echo "========================================"
echo ""
echo "建议:"
echo "  1. 运行 cmake --build build 确保编译通过"
echo "  2. 查看 TODO.md 了解待办事项"
echo "  3. 查看 REFACTORING_SUMMARY.md 了解重构进度"
echo ""
