// 覆盖 src/tool/file_read_tool.cpp 新增的 ToolSummary 字段与大文件 hint。
// 使用 std::filesystem::temp_directory_path() 建临时文件,保证测试跨平台。
// 场景:
//   1. 小文件(几百字节)读取 → summary 有 lines/size,不含 hint
//   2. 大文件(>200KB) 全量读取 → output 末尾有 hint 行, metrics 含 hint
//   3. 大文件带 start_line/end_line → 不触发 hint
//   4. 不存在的文件 → success=false;summary 目前不填充(走 legacy 渲染)

#include <gtest/gtest.h>

#include "tool/file_read_tool.hpp"
#include "tool/tool_executor.hpp"

#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using acecode::create_file_read_tool;
using acecode::ToolContext;
using acecode::ToolImpl;
using acecode::ToolResult;

namespace {

fs::path make_temp_file(const std::string& body) {
    // 每次测试用独立文件名,避免并行测试冲突。
    static std::atomic<int> seq{0};
    int n = ++seq;
    fs::path p = fs::temp_directory_path() /
                 ("acecode_file_read_summary_" + std::to_string(n) + ".txt");
    std::ofstream ofs(p, std::ios::binary);
    ofs << body;
    return p;
}

bool has_metric(const acecode::ToolSummary& s, const std::string& k) {
    for (const auto& kv : s.metrics) {
        if (kv.first == k) return true;
    }
    return false;
}

std::string get_metric(const acecode::ToolSummary& s, const std::string& k) {
    for (const auto& kv : s.metrics) {
        if (kv.first == k) return kv.second;
    }
    return {};
}

} // namespace

// 场景 1: 读几百字节的小文件,summary 应有 verb/object/lines/size,但没有 hint。
TEST(FileReadToolSummary, SmallFileNoHint) {
    ToolImpl tool = create_file_read_tool();

    auto p = make_temp_file("line1\nline2\nline3\n");
    nlohmann::json args = {{"file_path", p.string()}};
    ToolResult r = tool.execute(args.dump(), ToolContext{});

    ASSERT_TRUE(r.success);
    ASSERT_TRUE(r.summary.has_value());
    EXPECT_EQ(r.summary->verb, "Read");
    EXPECT_EQ(r.summary->object, p.string());
    EXPECT_EQ(get_metric(*r.summary, "lines"), "3");
    EXPECT_FALSE(get_metric(*r.summary, "size").empty());
    EXPECT_FALSE(has_metric(*r.summary, "hint"));
    EXPECT_EQ(r.output.find("[hint:"), std::string::npos);

    fs::remove(p);
}

// 场景 2: 读 > 200KB 的大文件且未限 range,应该在 output 末尾追加 hint 行,
// 且 metrics 含 ("hint", "large_file")。
TEST(FileReadToolSummary, LargeFileAppendsHint) {
    ToolImpl tool = create_file_read_tool();

    std::string body;
    body.reserve(250 * 1024);
    // 每行 100 字节 * 2600 行 ≈ 260KB
    for (int i = 0; i < 2600; ++i) {
        body.append(std::string(99, 'x')).append("\n");
    }
    auto p = make_temp_file(body);

    nlohmann::json args = {{"file_path", p.string()}};
    ToolResult r = tool.execute(args.dump(), ToolContext{});

    ASSERT_TRUE(r.success);
    ASSERT_TRUE(r.summary.has_value());
    EXPECT_EQ(get_metric(*r.summary, "hint"), "large_file");
    EXPECT_NE(r.output.find("[hint: file is large"), std::string::npos);

    fs::remove(p);
}

// 场景 3: 大文件 + 明确 start_line/end_line → 不追加 hint(调用方已经收窄了)。
TEST(FileReadToolSummary, LargeFileWithRangeNoHint) {
    ToolImpl tool = create_file_read_tool();

    std::string body;
    body.reserve(250 * 1024);
    for (int i = 0; i < 2600; ++i) {
        body.append(std::string(99, 'x')).append("\n");
    }
    auto p = make_temp_file(body);

    nlohmann::json args = {
        {"file_path", p.string()},
        {"start_line", 10},
        {"end_line", 20}
    };
    ToolResult r = tool.execute(args.dump(), ToolContext{});

    ASSERT_TRUE(r.success);
    ASSERT_TRUE(r.summary.has_value());
    EXPECT_FALSE(has_metric(*r.summary, "hint"));
    EXPECT_EQ(r.output.find("[hint:"), std::string::npos);

    fs::remove(p);
}

// 场景 4: 读不存在的文件 → success=false;此时我们不强求 summary,允许 TUI
// 走 legacy 渲染。这里断言错误路径不崩溃即可。
TEST(FileReadToolSummary, MissingFileDoesNotCrash) {
    ToolImpl tool = create_file_read_tool();

    nlohmann::json args = {{"file_path", "/this/path/definitely/does/not/exist/xyz.txt"}};
    ToolResult r = tool.execute(args.dump(), ToolContext{});

    EXPECT_FALSE(r.success);
}
