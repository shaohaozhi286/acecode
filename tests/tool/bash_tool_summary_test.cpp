// 覆盖 src/tool/bash_tool.cpp 新增的 ToolSummary 字段。
// 由于 bash_tool 直接 fork/exec 真实子进程,这里不能完全 mock —— 所以挑了
// 跨平台都有意义的最小命令 (echo / 错误退出码 / 产生大量输出) 做端到端断言。
// 场景:
//   1. 成功命令 → summary 含 verb / object / time / bytes,不含 exit
//   2. 失败命令 (exit 2) → success=false 且 metrics 含 ("exit", "2")
//   3. 超过 MAX_OUTPUT_SIZE 的大输出 → 标记 truncated=true 且输出里出现
//      `[... N bytes omitted ...]` 行 (仅 POSIX 下做,Windows cmd.exe
//      难以在一行里生成 100KB+ 输出,交给集成测试)

#include <gtest/gtest.h>

#include "tool/bash_tool.hpp"
#include "tool/tool_executor.hpp"

#include <nlohmann/json.hpp>
#include <chrono>
#include <string>

using acecode::create_bash_tool;
using acecode::ToolContext;
using acecode::ToolImpl;
using acecode::ToolResult;

namespace {

// 找到 summary 里第一个 key=k 的 value;找不到返回空。供断言。
std::string get_metric(const acecode::ToolSummary& s, const std::string& k) {
    for (const auto& kv : s.metrics) {
        if (kv.first == k) return kv.second;
    }
    return {};
}

bool has_metric(const acecode::ToolSummary& s, const std::string& k) {
    for (const auto& kv : s.metrics) {
        if (kv.first == k) return true;
    }
    return false;
}

} // namespace

// 场景 1: 运行 `echo hello` —— 最基本的成功路径,验证 summary 被填充且
// metrics 里应有 time/bytes,没有 exit/truncated/aborted。
TEST(BashToolSummary, SuccessfulEchoHasCoreMetrics) {
    ToolImpl tool = create_bash_tool();

    nlohmann::json args = {{"command", "echo hello"}};
    ToolContext ctx;
    ToolResult r = tool.execute(args.dump(), ctx);

    ASSERT_TRUE(r.summary.has_value());
    EXPECT_EQ(r.summary->verb, "Ran");
    EXPECT_EQ(r.summary->object, "echo hello");
    EXPECT_TRUE(r.success);
    EXPECT_FALSE(get_metric(*r.summary, "time").empty());
    EXPECT_FALSE(get_metric(*r.summary, "bytes").empty());
    EXPECT_FALSE(has_metric(*r.summary, "exit"));
    EXPECT_FALSE(has_metric(*r.summary, "truncated"));
    EXPECT_FALSE(has_metric(*r.summary, "aborted"));
}

// 场景 2: 运行一个以 exit 2 结束的命令,验证 success=false 且 metrics 含
// ("exit", "2")。跨平台 sh 和 cmd 都接受 `exit 2` 语义。
TEST(BashToolSummary, FailedExitCodeAppearsInMetrics) {
    ToolImpl tool = create_bash_tool();

    nlohmann::json args = {{"command", "exit 2"}};
    ToolContext ctx;
    ToolResult r = tool.execute(args.dump(), ctx);

    ASSERT_TRUE(r.summary.has_value());
    EXPECT_FALSE(r.success);
    EXPECT_EQ(get_metric(*r.summary, "exit"), "2");
}

#ifdef _WIN32
// 场景 3 (Windows only,回归测试,2026-05-08 由 agent-browser 触发):
//
// **触发场景**:bash_tool 直接 spawn 的子进程(cmd.exe)在 spawn 完一个会继续
//   存活的孙进程后立即退出。`start /B cmd /c "timeout /t 8 /nobreak >nul"`
//   是最简单的复现:`start /B` 把内层 cmd 放到后台并继承父 cmd 的标准输出/错误
//   管道写端;父 cmd 跑完后续 `&& echo done` 后立刻 exit,但内层 cmd 仍 hold
//   管道写端 8 秒。真实场景:`agent-browser open https://...` 启动 Chrome 后退出,
//   Chrome 接管管道写端继续运行。
//
// **期望行为**:bash_tool 在父 cmd 退出后**瞬时返回**(< 4s),输出里包含 "done"
//   (说明父 cmd 的 echo 被正确捕获),success=true。后台孙进程的命运交给 OS,
//   bash_tool 不等。
//
// **修复前 bug 表现**:`bash_tool.cpp:191-198` 的"进程正常退出 drain"分支裸调
//   ReadFile,Windows 同步 ReadFile 在管道还有任何写端持有者时会**永久阻塞**等
//   EOF。这条 drain 在 abort_flag / timeout 检查的**外面**,所以 Esc 失效、120s
//   timeout 失效,TUI 表面"卡死"直到孙进程自己退出(本测试约 8s,用户的真实
//   case 是 36 分钟还没退)。
//
// **阈值 4000ms 的依据**:本机实测修复后命中 30-100ms 区间;CI / Windows Defender
//   扫描子进程偶尔会拖到 1-2s,4s 给足缓冲。回归时会回到 ~8000ms 量级(等孙进程
//   自然退出),4s 一刀切干净。
TEST(BashToolWinPipeDrain, ReturnsPromptlyWhenGrandchildHoldsPipe) {
    ToolImpl tool = create_bash_tool();

    nlohmann::json args = {
        {"command", "start /B cmd /c \"timeout /t 8 /nobreak >nul\" && echo done"},
        {"timeout_ms", 30000},
    };
    ToolContext ctx;

    auto t0 = std::chrono::steady_clock::now();
    ToolResult r = tool.execute(args.dump(), ctx);
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    EXPECT_LT(elapsed_ms, 4000)
        << "bash_tool blocked in post-exit drain due to grandchild holding pipe write end; "
           "actual elapsed=" << elapsed_ms << "ms";
    EXPECT_TRUE(r.success);
    // 父 cmd 在 echo done 后 exit;输出里应该看到 "done"。
    EXPECT_NE(r.output.find("done"), std::string::npos);
}
#endif

#ifndef _WIN32
// 场景 4 (POSIX only): 用 `head -c 150000 /dev/zero` 生成超过 100KB 的输出,
// 应触发 head+tail 截断并在 output 里插入 marker。
TEST(BashToolSummary, LargeOutputTriggersHeadTailTruncation) {
    ToolImpl tool = create_bash_tool();

    // /dev/zero 写二进制 NUL;bash_tool 会经 strip_ansi 但 \0 不是 ANSI
    // 控制符,会被原样保留 (最终 ensure_utf8 可能做替换)。我们这里只关心
    // 体积超过 100KB → 触发截断。
    nlohmann::json args = {{"command", "head -c 150000 /dev/zero | cat"}};
    ToolContext ctx;
    ToolResult r = tool.execute(args.dump(), ctx);

    ASSERT_TRUE(r.summary.has_value());
    EXPECT_EQ(get_metric(*r.summary, "truncated"), "true");
    EXPECT_NE(r.output.find("bytes omitted"), std::string::npos);
}
#endif
