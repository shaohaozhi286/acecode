#include <gtest/gtest.h>

#include "agent_loop_shell_guard.hpp"

#include <string>

using acecode::command_looks_like_file_write;
using acecode::command_mentions_path;

// 回归测试:file_edit 安全失败后,提及该文件的 bash 命令会被 "Shell write blocked"
// 拦截。修复前 command_looks_like_file_write 把任何含 "powershell"/"python" 的命令
// 一律当成写入,导致纯读取命令(模型在编辑失败后用 Get-Content 求证文件状态)也被拦,
// 模型失去最后一条求证通道。期望:无写入特征的解释器读取命令不算写入。
TEST(ShellWriteGuard, PureReadInterpreterCommandIsNotWrite) {
    EXPECT_FALSE(command_looks_like_file_write(
        R"ps(powershell -Command "(Get-Content 'D:/code/web/src/common/i18n/en.js' -TotalCount 2540 | Select-Object -Last 15) -join [Environment]::NewLine")ps"));
    EXPECT_FALSE(command_looks_like_file_write(
        R"ps(python -c "print(open('a.txt').read())")ps"));
    EXPECT_FALSE(command_looks_like_file_write("cat src/app.cpp"));
}

// 守住防绕写能力:显式写入特征(重定向 / 写文件 cmdlet / sed 原地改 / 脚本写文件 API)
// 必须仍被识别为写入——这些正是模型绕过 file_edit 文本安全的常见手段。
TEST(ShellWriteGuard, ExplicitWriteMarkersAreDetected) {
    EXPECT_TRUE(command_looks_like_file_write("echo hi > out.txt"));
    EXPECT_TRUE(command_looks_like_file_write(
        R"ps(powershell -Command "Get-Content a.txt | Set-Content b.txt")ps"));
    EXPECT_TRUE(command_looks_like_file_write(
        R"ps(powershell -Command "'x' | Out-File a.txt")ps"));
    EXPECT_TRUE(command_looks_like_file_write("sed -i s/a/b/ file.txt"));
    EXPECT_TRUE(command_looks_like_file_write(
        R"ps(powershell -Command "[IO.File]::WriteAllText('a.txt','x')")ps"));
    EXPECT_TRUE(command_looks_like_file_write(
        R"ps(python -c "open('a.txt','w').write('x')")ps"));
    EXPECT_TRUE(command_looks_like_file_write(
        R"ps(python -c "from pathlib import Path; Path('a.txt').write_text('x')")ps"));
}

// 路径提及判定的基本行为:完整路径(大小写/斜杠方向不敏感)与 basename 兜底匹配。
// basename 长度阈值 4 来自原实现:过短的文件名(如 "a.js")在命令里误匹配概率太高。
TEST(ShellWriteGuard, CommandMentionsPathMatchesFullPathAndBasename) {
    EXPECT_TRUE(command_mentions_path(
        R"ps(type D:\Code\web\src\common\i18n\en.js)ps",
        "d:/code/web/src/common/i18n/en.js"));
    EXPECT_TRUE(command_mentions_path(
        "cat zhTraditional.js", "D:/code/web/src/common/i18n/zhTraditional.js"));
    EXPECT_FALSE(command_mentions_path("echo hello", "D:/code/web/src/app.js"));
    EXPECT_FALSE(command_mentions_path("anything", ""));
}
