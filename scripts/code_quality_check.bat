@echo off
REM AceCode - 代码整理检查脚本
REM 用于检测常见的DRY违规和代码质量问题

echo ========================================
echo AceCode 代码质量检查
echo ========================================
echo.

echo [1/6] 检查重复的JSON解析模式...
findstr /S /C:"json::parse(arguments_json)" src\*.cpp | find /C "json::parse" > nul
if %ERRORLEVEL% EQU 0 (
    echo ⚠ 发现未使用ToolArgsParser的文件:
    findstr /S /N /C:"json::parse(arguments_json)" src\*.cpp
) else (
    echo ✓ 所有工具都使用了ToolArgsParser
)
echo.

echo [2/6] 检查硬编码的错误消息...
findstr /S /C:"[Error]" src\tool\*.cpp | find /C "[Error]" > nul
if %ERRORLEVEL% EQU 0 (
    echo ⚠ 发现硬编码的错误消息:
    findstr /S /N /C:"[Error]" src\tool\*.cpp | findstr /V "ToolErrors::"
) else (
    echo ✓ 所有错误消息都使用了ToolErrors类
)
echo.

echo [3/6] 检查magic numbers...
echo ⚠ 搜索常见的magic numbers:
findstr /S /N /C:"/ 4" src\*.cpp | findstr /V "constants::" | findstr /V "//"
findstr /S /N "10 \* 1024 \* 1024" src\*.cpp | findstr /V "constants::"
findstr /S /N "100 \* 1024" src\*.cpp | findstr /V "constants::"
echo.

echo [4/6] 检查TODO/FIXME注释...
findstr /S /N /C:"TODO" /C:"FIXME" /C:"XXX" src\*.cpp src\*.hpp 2>nul
if %ERRORLEVEL% NEQ 0 echo ✓ 未发现TODO/FIXME注释
echo.

echo [5/6] 检查过长的函数...
echo ⚠ 检查超过200行的函数 (需要手动检查main.cpp等)
echo   - main.cpp 可能包含过长函数
echo   - agent_loop.cpp 的submit()可能过长
echo.

echo [6/6] 统计代码行数...
for /f %%a in ('dir /s /b src\*.cpp ^| find /C ".cpp"') do set CPP_COUNT=%%a
for /f %%a in ('dir /s /b src\*.hpp ^| find /C ".hpp"') do set HPP_COUNT=%%a
echo   - .cpp文件: %CPP_COUNT%个
echo   - .hpp文件: %HPP_COUNT%个
echo.

echo ========================================
echo 检查完成
echo ========================================
echo.
echo 建议:
echo   1. 运行 cmake --build build 确保编译通过
echo   2. 查看 TODO.md 了解待办事项
echo   3. 查看 REFACTORING_SUMMARY.md 了解重构进度
echo.

pause
