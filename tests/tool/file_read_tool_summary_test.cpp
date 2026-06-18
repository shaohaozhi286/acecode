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
#include <atomic>
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

TEST(FileReadToolSummary, OutputIncludesReadMetadataFooterWithoutEditHashes) {
    ToolImpl tool = create_file_read_tool();

    auto p = make_temp_file("line1\r\nline2\r\n");
    nlohmann::json args = {
        {"file_path", p.string()},
        {"start_line", 2},
        {"end_line", 2}
    };
    ToolResult r = tool.execute(args.dump(), ToolContext{});

    ASSERT_TRUE(r.success) << r.output;
    EXPECT_NE(r.output.find("2: line2\n"), std::string::npos);
    EXPECT_NE(r.output.find("acecode-read-metadata"), std::string::npos);
    EXPECT_NE(r.output.find("encoding=\"utf-8\""), std::string::npos);
    EXPECT_NE(r.output.find("line_endings=\"crlf\""), std::string::npos);
    EXPECT_NE(r.output.find("range=\"2-2\""), std::string::npos);
    EXPECT_EQ(r.output.find("range_hash=\"sha256:"), std::string::npos);
    EXPECT_EQ(r.output.find("read_id=\""), std::string::npos);

    fs::remove(p);
}

TEST(FileReadToolSummary, LossyReadReportsReplacementCountAndNoEditHash) {
    ToolImpl tool = create_file_read_tool();

    std::string body = std::string(u8"中文日志\n");
    body.push_back(static_cast<char>(0xE4));
    auto p = make_temp_file(body);

    ToolResult r = tool.execute(nlohmann::json({{"file_path", p.string()}}).dump(),
                                ToolContext{});

    ASSERT_TRUE(r.success) << r.output;
    ASSERT_TRUE(r.summary.has_value());
    EXPECT_NE(r.output.find(std::string(u8"中文日志")), std::string::npos);
    EXPECT_NE(r.output.find("[note: decoded with 1 replacement(s)"), std::string::npos);
    EXPECT_NE(r.output.find("encoding=\"utf-8 (lossy)\""), std::string::npos);
    EXPECT_NE(r.output.find("lossy=\"true\""), std::string::npos);
    EXPECT_NE(r.output.find("editable=\"false\""), std::string::npos);
    EXPECT_EQ(r.output.find("range_hash=\"sha256:"), std::string::npos);
    EXPECT_EQ(get_metric(*r.summary, "enc"), "utf-8 (lossy)");
    EXPECT_EQ(get_metric(*r.summary, "lossy"), "1");

    fs::remove(p);
}

TEST(FileReadToolSummary, PartialLossyReadStillAvoidsEditableRangeMetadata) {
    ToolImpl tool = create_file_read_tool();

    std::string body = std::string(u8"第一行\n第二行\n");
    body.push_back(static_cast<char>(0xE4));
    auto p = make_temp_file(body);

    ToolResult r = tool.execute(nlohmann::json({
        {"file_path", p.string()},
        {"start_line", 1},
        {"end_line", 1}
    }).dump(), ToolContext{});

    ASSERT_TRUE(r.success) << r.output;
    EXPECT_NE(r.output.find(std::string(u8"1: 第一行\n")), std::string::npos);
    EXPECT_NE(r.output.find("[note: decoded with 1 replacement(s)"), std::string::npos);
    EXPECT_NE(r.output.find("lossy=\"true\""), std::string::npos);
    EXPECT_EQ(r.output.find("range=\"1-1\""), std::string::npos);
    EXPECT_EQ(r.output.find("range_hash=\"sha256:"), std::string::npos);

    fs::remove(p);
}

#ifdef _WIN32
TEST(FileReadToolSummary, ReadsGbkAsUtf8) {
    ToolImpl tool = create_file_read_tool();

    auto p = fs::temp_directory_path() / "acecode_file_read_summary_gbk.txt";
    {
        std::ofstream ofs(p, std::ios::binary);
        ofs << std::string("\xD6\xD0\xCE\xC4\n", 5); // "中文\n" in CP936
    }

    ToolResult r = tool.execute(nlohmann::json({{"file_path", p.string()}}).dump(),
                                ToolContext{});

    ASSERT_TRUE(r.success) << r.output;
    EXPECT_NE(r.output.find(std::string(u8"中文")), std::string::npos);
    EXPECT_NE(r.output.find("encoding=\"gbk/cp936\""), std::string::npos);

    fs::remove(p);
}
#endif
