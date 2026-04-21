// 覆盖 src/tool/file_write_tool.cpp 新增的 ToolSummary 字段。
// 场景:
//   1. 新建文件 → verb=Created, metrics=[("+", N)]
//   2. 覆盖文件 → verb=Wrote, metrics=[("+", a), ("-", d)],利用 DiffStats
//   3. 错误路径 (比如 file_path 为空) → success=false,不崩溃

#include <gtest/gtest.h>

#include "tool/file_write_tool.hpp"
#include "tool/tool_executor.hpp"

#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using acecode::create_file_write_tool;
using acecode::ToolContext;
using acecode::ToolImpl;
using acecode::ToolResult;

namespace {

fs::path fresh_temp_path(const std::string& suffix) {
    static std::atomic<int> seq{0};
    return fs::temp_directory_path() /
           ("acecode_file_write_summary_" + std::to_string(++seq) + suffix);
}

std::string get_metric(const acecode::ToolSummary& s, const std::string& k) {
    for (const auto& kv : s.metrics) {
        if (kv.first == k) return kv.second;
    }
    return {};
}

} // namespace

// 场景 1: 往一个不存在的路径写 100 行,summary.verb 应为 Created,
// metrics 的 "+" 应等于 100。
TEST(FileWriteToolSummary, CreatedFileReportsLineCount) {
    ToolImpl tool = create_file_write_tool();

    auto p = fresh_temp_path(".txt");
    if (fs::exists(p)) fs::remove(p);

    std::string body;
    for (int i = 0; i < 100; ++i) body += "line\n";

    nlohmann::json args = {
        {"file_path", p.string()},
        {"content", body}
    };
    ToolResult r = tool.execute(args.dump(), ToolContext{});

    ASSERT_TRUE(r.success);
    ASSERT_TRUE(r.summary.has_value());
    EXPECT_EQ(r.summary->verb, "Created");
    EXPECT_EQ(r.summary->object, p.string());
    EXPECT_EQ(get_metric(*r.summary, "+"), "100");

    fs::remove(p);
}

// 场景 2: 覆盖一个已有文件,修改若干行,验证 verb=Wrote 且 +/- 计数正确。
// 旧文件 3 行 (a/b/c),新内容 4 行 (a/X/Y/c),期待 +2 / -1。
TEST(FileWriteToolSummary, OverwriteReportsDiffStats) {
    ToolImpl tool = create_file_write_tool();

    auto p = fresh_temp_path(".txt");
    {
        std::ofstream ofs(p, std::ios::binary);
        ofs << "a\nb\nc\n";
    }

    nlohmann::json args = {
        {"file_path", p.string()},
        {"content", "a\nX\nY\nc\n"}
    };
    ToolResult r = tool.execute(args.dump(), ToolContext{});

    ASSERT_TRUE(r.success);
    ASSERT_TRUE(r.summary.has_value());
    EXPECT_EQ(r.summary->verb, "Wrote");
    EXPECT_EQ(get_metric(*r.summary, "+"), "2");
    EXPECT_EQ(get_metric(*r.summary, "-"), "1");

    fs::remove(p);
}

// 场景 3: file_path 缺失 → success=false,路径非法不应 crash。
TEST(FileWriteToolSummary, MissingFilePathDoesNotCrash) {
    ToolImpl tool = create_file_write_tool();

    nlohmann::json args = {{"content", "hello"}};
    ToolResult r = tool.execute(args.dump(), ToolContext{});

    EXPECT_FALSE(r.success);
}
