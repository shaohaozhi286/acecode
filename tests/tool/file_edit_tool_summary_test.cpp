// 覆盖 src/tool/file_edit_tool.cpp 新增的 ToolSummary 字段。
// 场景:
//   1. 替换单行 (old_string 命中一次) → verb=Edited,+1 -1
//   2. 替换多行 (old 2 行 → new 3 行) → verb=Edited,+3 -2
//   3. anchor 未命中 → success=false,不崩溃(允许无 summary)

#include <gtest/gtest.h>

#include "tool/file_edit_tool.hpp"
#include "tool/tool_executor.hpp"

#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using acecode::create_file_edit_tool;
using acecode::ToolContext;
using acecode::ToolImpl;
using acecode::ToolResult;

namespace {

fs::path fresh_temp_path() {
    static std::atomic<int> seq{0};
    return fs::temp_directory_path() /
           ("acecode_file_edit_summary_" + std::to_string(++seq) + ".txt");
}

std::string get_metric(const acecode::ToolSummary& s, const std::string& k) {
    for (const auto& kv : s.metrics) {
        if (kv.first == k) return kv.second;
    }
    return {};
}

} // namespace

// 场景 1: 单行替换。文件内容 "a\nOLD\nc\n",把 OLD 替换成 NEW。
// 期待 +1 / -1。
TEST(FileEditToolSummary, SingleLineReplacementStats) {
    ToolImpl tool = create_file_edit_tool();

    auto p = fresh_temp_path();
    {
        std::ofstream ofs(p, std::ios::binary);
        ofs << "a\nOLD\nc\n";
    }

    nlohmann::json args = {
        {"file_path", p.string()},
        {"old_string", "OLD"},
        {"new_string", "NEW"}
    };
    ToolResult r = tool.execute(args.dump(), ToolContext{});

    ASSERT_TRUE(r.success);
    ASSERT_TRUE(r.summary.has_value());
    EXPECT_EQ(r.summary->verb, "Edited");
    EXPECT_EQ(r.summary->object, p.string());
    EXPECT_EQ(get_metric(*r.summary, "+"), "1");
    EXPECT_EQ(get_metric(*r.summary, "-"), "1");

    fs::remove(p);
}

// 场景 2: 多行替换。把 "X\nY\n" 换成 "A\nB\nC\n" → +3 / -2。
TEST(FileEditToolSummary, MultilineReplacementStats) {
    ToolImpl tool = create_file_edit_tool();

    auto p = fresh_temp_path();
    {
        std::ofstream ofs(p, std::ios::binary);
        ofs << "prefix\nX\nY\nsuffix\n";
    }

    nlohmann::json args = {
        {"file_path", p.string()},
        {"old_string", "X\nY\n"},
        {"new_string", "A\nB\nC\n"}
    };
    ToolResult r = tool.execute(args.dump(), ToolContext{});

    ASSERT_TRUE(r.success);
    ASSERT_TRUE(r.summary.has_value());
    EXPECT_EQ(get_metric(*r.summary, "+"), "3");
    EXPECT_EQ(get_metric(*r.summary, "-"), "2");

    fs::remove(p);
}

// 场景 3: old_string 在文件中不存在 → success=false。
// 此时我们不强求 summary(错误文案已自带诊断信息),主要断言不崩溃。
TEST(FileEditToolSummary, AnchorNotFoundFails) {
    ToolImpl tool = create_file_edit_tool();

    auto p = fresh_temp_path();
    {
        std::ofstream ofs(p, std::ios::binary);
        ofs << "hello world\n";
    }

    nlohmann::json args = {
        {"file_path", p.string()},
        {"old_string", "NOT_THERE"},
        {"new_string", "X"}
    };
    ToolResult r = tool.execute(args.dump(), ToolContext{});

    EXPECT_FALSE(r.success);

    fs::remove(p);
}
