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

#ifndef _WIN32
// 场景 3 (POSIX only): 用 `head -c 150000 /dev/zero` 生成超过 100KB 的输出,
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
