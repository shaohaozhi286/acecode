// 覆盖 src/tool/file_read_tool.cpp 新增的 ToolSummary 字段与大文件 hint。
// 使用 std::filesystem::temp_directory_path() 建临时文件,保证测试跨平台。
// 场景:
//   1. 小文件(几百字节)读取 → summary 有 lines/size,不含 hint
//   2. 大文件(>200KB) 全量读取 → output 末尾有 hint 行, metrics 含 hint
//   3. 大文件带 start_line/end_line → 不触发 hint
//   4. 不存在的文件 → success=false;summary 目前不填充(走 legacy 渲染)

#include <gtest/gtest.h>

#include "tool/file_edit_tool.hpp"
#include "tool/file_read_tool.hpp"
#include "tool/file_write_tool.hpp"
#include "tool/mtime_tracker.hpp"
#include "tool/tool_executor.hpp"

#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using acecode::create_file_read_tool;
using acecode::create_file_edit_tool;
using acecode::create_file_write_tool;
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

TEST(FileReadToolSummary, DescriptionWarnsAgainstRedundantSameRangeReads) {
    ToolImpl tool = create_file_read_tool();

    EXPECT_NE(tool.definition.description.find("Do not re-read the same file/range"),
              std::string::npos);
    EXPECT_NE(tool.definition.description.find("repeated unchanged reads return a compact stub"),
              std::string::npos);
}

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
    EXPECT_NE(r.output.find("partial=\"false\""), std::string::npos);

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

TEST(FileReadToolSummary, ReadsRangeBeyondTenMiBWithoutWholeFileRejection) {
    ToolImpl tool = create_file_read_tool();

    auto p = make_temp_file("");
    {
        std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
        const std::string padding(90, 'x');
        for (int line = 1; line <= 120000; ++line) {
            if (line == 115000) {
                ofs << "target-large-file";
            } else {
                ofs << "ordinary";
            }
            ofs << padding << "\n";
        }
    }
    ASSERT_GT(fs::file_size(p), 10u * 1024u * 1024u);

    ToolResult r = tool.execute(nlohmann::json({
        {"file_path", p.string()},
        {"start_line", 115000},
        {"end_line", 115000}
    }).dump(), ToolContext{});

    ASSERT_TRUE(r.success) << r.output;
    EXPECT_NE(r.output.find("115000: target-large-file"), std::string::npos);
    EXPECT_NE(r.output.find("partial=\"true\""), std::string::npos);
    EXPECT_EQ(r.output.find("File too large"), std::string::npos);

    fs::remove(p);
}

TEST(FileReadToolSummary, UnboundedReadStopsAtContentLimitWithByteContinuation) {
    ToolImpl tool = create_file_read_tool();

    auto p = make_temp_file(std::string(60000, 'z'));
    ToolResult r = tool.execute(
        nlohmann::json({{"file_path", p.string()}}).dump(),
        ToolContext{});

    ASSERT_TRUE(r.success) << r.output;
    EXPECT_LT(r.output.size(), 50u * 1024u);
    EXPECT_NE(r.output.find("truncated=\"true\""), std::string::npos);
    EXPECT_NE(r.output.find("partial=\"true\""), std::string::npos);
    EXPECT_NE(r.output.find("next_byte_offset=\"49152\""), std::string::npos);
    EXPECT_NE(r.output.find("Continue with byte_offset=49152"), std::string::npos);

    fs::remove(p);
}

TEST(FileReadToolSummary, LargeSingleLineUsesBoundedByteContinuation) {
    ToolImpl tool = create_file_read_tool();

    auto p = make_temp_file("");
    {
        std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
        const std::string block(1024 * 1024, 'q');
        for (int i = 0; i < 11; ++i) ofs << block;
    }
    ASSERT_GT(fs::file_size(p), 10u * 1024u * 1024u);

    ToolResult r = tool.execute(
        nlohmann::json({{"file_path", p.string()}}).dump(),
        ToolContext{});

    ASSERT_TRUE(r.success) << r.output;
    EXPECT_LT(r.output.size(), 50u * 1024u);
    EXPECT_NE(r.output.find("truncated=\"true\""), std::string::npos);
    EXPECT_NE(r.output.find("next_byte_offset=\"49152\""), std::string::npos);

    fs::remove(p);
}

TEST(FileReadToolSummary, ByteWindowsExposeExactIntervalsAndDoNotDeduplicateOffsets) {
    ToolImpl tool = create_file_read_tool();

    auto p = make_temp_file("0123456789abcdef");
    ToolResult first = tool.execute(nlohmann::json({
        {"file_path", p.string()},
        {"byte_offset", 4},
        {"max_bytes", 6}
    }).dump(), ToolContext{});
    ASSERT_TRUE(first.success) << first.output;
    EXPECT_NE(first.output.find("456789"), std::string::npos);
    EXPECT_NE(first.output.find("byte_range=\"4-10\""), std::string::npos);
    EXPECT_NE(first.output.find("next_byte_offset=\"10\""), std::string::npos);
    EXPECT_NE(first.output.find("partial=\"true\""), std::string::npos);

    ToolResult second = tool.execute(nlohmann::json({
        {"file_path", p.string()},
        {"byte_offset", 10},
        {"max_bytes", 6}
    }).dump(), ToolContext{});
    ASSERT_TRUE(second.success) << second.output;
    EXPECT_NE(second.output.find("abcdef"), std::string::npos);
    EXPECT_NE(second.output.find("byte_range=\"10-16\""), std::string::npos);
    EXPECT_EQ(second.output.find("File unchanged since last read"), std::string::npos);

    fs::remove(p);
}

TEST(FileReadToolSummary, ByteWindowContinuationDoesNotSkipSplitUtf8Character) {
    ToolImpl tool = create_file_read_tool();

    auto p = make_temp_file(std::string(32767, 'a') + u8"中tail");
    ToolResult first = tool.execute(nlohmann::json({
        {"file_path", p.string()},
        {"byte_offset", 0}
    }).dump(), ToolContext{});
    ASSERT_TRUE(first.success) << first.output;
    EXPECT_NE(first.output.find("byte_range=\"0-32767\""), std::string::npos);
    EXPECT_NE(first.output.find("next_byte_offset=\"32767\""), std::string::npos);

    ToolResult second = tool.execute(nlohmann::json({
        {"file_path", p.string()},
        {"byte_offset", 32767},
        {"max_bytes", 7}
    }).dump(), ToolContext{});
    ASSERT_TRUE(second.success) << second.output;
    EXPECT_NE(second.output.find(u8"中tail"), std::string::npos);
    EXPECT_NE(second.output.find("byte_range=\"32767-32774\""), std::string::npos);

    fs::remove(p);
}

TEST(FileReadToolSummary, RejectsConflictingOrOversizedByteWindowArguments) {
    ToolImpl tool = create_file_read_tool();
    auto p = make_temp_file("alpha\n");

    ToolResult conflict = tool.execute(nlohmann::json({
        {"file_path", p.string()},
        {"start_line", 1},
        {"byte_offset", 0}
    }).dump(), ToolContext{});
    EXPECT_FALSE(conflict.success);
    EXPECT_NE(conflict.output.find("cannot be combined"), std::string::npos);

    ToolResult oversized = tool.execute(nlohmann::json({
        {"file_path", p.string()},
        {"byte_offset", 0},
        {"max_bytes", 32769}
    }).dump(), ToolContext{});
    EXPECT_FALSE(oversized.success);
    EXPECT_NE(oversized.output.find("between 1 and 32768"), std::string::npos);

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

TEST(FileReadToolSummary, DuplicateFullReadReturnsUnchangedStub) {
    ToolImpl tool = create_file_read_tool();

    auto p = make_temp_file("alpha\nbeta\n");
    nlohmann::json args = {{"file_path", p.string()}};

    ToolResult first = tool.execute(args.dump(), ToolContext{});
    ASSERT_TRUE(first.success) << first.output;
    EXPECT_NE(first.output.find("alpha\nbeta\n"), std::string::npos);

    ToolResult second = tool.execute(args.dump(), ToolContext{});
    ASSERT_TRUE(second.success) << second.output;
    EXPECT_NE(second.output.find("File unchanged since last read"), std::string::npos);
    EXPECT_EQ(second.output.find("alpha\nbeta\n"), std::string::npos);
    ASSERT_TRUE(second.summary.has_value());
    EXPECT_EQ(get_metric(*second.summary, "cache"), "unchanged");

    fs::remove(p);
}

TEST(FileReadToolSummary, DuplicateReadStubIncludesPreviousPersistedReference) {
    ToolImpl tool = create_file_read_tool();

    auto p = make_temp_file("alpha\nbeta\n");
    nlohmann::json args = {{"file_path", p.string()}};

    ToolResult first = tool.execute(args.dump(), ToolContext{});
    ASSERT_TRUE(first.success) << first.output;
    acecode::MtimeTracker::instance().record_read_observation_result(
        p.string(), 0, 0, "call-read-original",
        "C:\\Users\\shao\\.acecode\\projects\\hash\\sid\\tool-results\\call-read-original.txt");

    ToolResult second = tool.execute(args.dump(), ToolContext{});
    ASSERT_TRUE(second.success) << second.output;
    EXPECT_NE(second.output.find("File unchanged since last read"), std::string::npos);
    EXPECT_NE(second.output.find("Previous file_read tool_call_id: call-read-original"),
              std::string::npos);
    EXPECT_NE(second.output.find("Full previous output path: C:\\Users\\shao\\.acecode"),
              std::string::npos);
    EXPECT_NE(second.output.find("call file_read on that saved output path"),
              std::string::npos);
    EXPECT_EQ(second.output.find("alpha\nbeta\n"), std::string::npos);

    fs::remove(p);
}

TEST(FileReadToolSummary, ClearingReadObservationsPreservesEditBaselineAndAllowsRealRead) {
    ToolImpl tool = create_file_read_tool();

    auto p = make_temp_file("alpha\nbeta\n");
    nlohmann::json args = {{"file_path", p.string()}};

    ToolResult first = tool.execute(args.dump(), ToolContext{});
    ASSERT_TRUE(first.success) << first.output;
    EXPECT_NE(first.output.find("alpha\nbeta\n"), std::string::npos);

    ToolResult cached = tool.execute(args.dump(), ToolContext{});
    ASSERT_TRUE(cached.success) << cached.output;
    EXPECT_NE(cached.output.find("File unchanged since last read"), std::string::npos);

    acecode::MtimeTracker::instance().clear_read_observations();

    auto baseline = acecode::MtimeTracker::instance().validate_read_baseline_for_edit(
        p.string(), "alpha\nbeta\n");
    EXPECT_EQ(baseline.status, acecode::MtimeTracker::ReadBaselineStatus::Ok);

    ToolResult after_clear = tool.execute(args.dump(), ToolContext{});
    ASSERT_TRUE(after_clear.success) << after_clear.output;
    EXPECT_NE(after_clear.output.find("alpha\nbeta\n"), std::string::npos);
    EXPECT_EQ(after_clear.output.find("File unchanged since last read"), std::string::npos);

    fs::remove(p);
}

TEST(FileReadToolSummary, DifferentRequestedRangeDoesNotDeduplicate) {
    ToolImpl tool = create_file_read_tool();

    auto p = make_temp_file("alpha\nbeta\ngamma\n");
    ToolResult first = tool.execute(nlohmann::json({
        {"file_path", p.string()},
        {"start_line", 1},
        {"end_line", 1}
    }).dump(), ToolContext{});
    ASSERT_TRUE(first.success) << first.output;
    EXPECT_NE(first.output.find("1: alpha\n"), std::string::npos);

    ToolResult second = tool.execute(nlohmann::json({
        {"file_path", p.string()},
        {"start_line", 2},
        {"end_line", 2}
    }).dump(), ToolContext{});
    ASSERT_TRUE(second.success) << second.output;
    EXPECT_NE(second.output.find("2: beta\n"), std::string::npos);
    EXPECT_EQ(second.output.find("File unchanged since last read"), std::string::npos);

    fs::remove(p);
}

TEST(FileReadToolSummary, ChangedFileFallsBackToCurrentContent) {
    ToolImpl tool = create_file_read_tool();

    auto p = make_temp_file("old\n");
    ToolResult first = tool.execute(nlohmann::json({{"file_path", p.string()}}).dump(),
                                    ToolContext{});
    ASSERT_TRUE(first.success) << first.output;

    {
        std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
        ofs << "new\n";
    }
    auto previous_time = fs::last_write_time(p);
    fs::last_write_time(p, previous_time + std::chrono::seconds(2));

    ToolResult second = tool.execute(nlohmann::json({{"file_path", p.string()}}).dump(),
                                     ToolContext{});
    ASSERT_TRUE(second.success) << second.output;
    EXPECT_NE(second.output.find("new\n"), std::string::npos);
    EXPECT_EQ(second.output.find("File unchanged since last read"), std::string::npos);

    fs::remove(p);
}

TEST(FileReadToolSummary, EquivalentPathSpellingUsesSameReadObservation) {
    ToolImpl tool = create_file_read_tool();

    auto p = make_temp_file("same\n");
    ToolResult first = tool.execute(nlohmann::json({{"file_path", p.string()}}).dump(),
                                    ToolContext{});
    ASSERT_TRUE(first.success) << first.output;

    fs::path variant = p.parent_path() / "." / p.filename();
    ToolResult second = tool.execute(nlohmann::json({{"file_path", variant.string()}}).dump(),
                                     ToolContext{});
    ASSERT_TRUE(second.success) << second.output;
    EXPECT_NE(second.output.find("File unchanged since last read"), std::string::npos);
    EXPECT_EQ(second.output.find("same\n"), std::string::npos);

    fs::remove(p);
}

TEST(FileReadToolSummary, FileWriteInvalidatesPriorReadObservation) {
    ToolImpl read_tool = create_file_read_tool();
    ToolImpl write_tool = create_file_write_tool();

    auto p = make_temp_file("old\n");
    ToolResult first = read_tool.execute(nlohmann::json({{"file_path", p.string()}}).dump(),
                                         ToolContext{});
    ASSERT_TRUE(first.success) << first.output;

    ToolResult write = write_tool.execute(nlohmann::json({
        {"file_path", p.string()},
        {"content", "new\n"}
    }).dump(), ToolContext{});
    ASSERT_TRUE(write.success) << write.output;

    ToolResult after_write = read_tool.execute(nlohmann::json({{"file_path", p.string()}}).dump(),
                                               ToolContext{});
    ASSERT_TRUE(after_write.success) << after_write.output;
    EXPECT_NE(after_write.output.find("new\n"), std::string::npos);
    EXPECT_EQ(after_write.output.find("File unchanged since last read"), std::string::npos);

    ToolResult repeated = read_tool.execute(nlohmann::json({{"file_path", p.string()}}).dump(),
                                            ToolContext{});
    ASSERT_TRUE(repeated.success) << repeated.output;
    EXPECT_NE(repeated.output.find("File unchanged since last read"), std::string::npos);
    EXPECT_EQ(repeated.output.find("new\n"), std::string::npos);

    fs::remove(p);
}

TEST(FileReadToolSummary, FileEditInvalidatesPriorReadObservation) {
    ToolImpl read_tool = create_file_read_tool();
    ToolImpl edit_tool = create_file_edit_tool();

    auto p = make_temp_file("alpha\nbeta\n");
    ToolResult first = read_tool.execute(nlohmann::json({{"file_path", p.string()}}).dump(),
                                         ToolContext{});
    ASSERT_TRUE(first.success) << first.output;

    ToolResult edit = edit_tool.execute(nlohmann::json({
        {"file_path", p.string()},
        {"old_string", "beta"},
        {"new_string", "delta"}
    }).dump(), ToolContext{});
    ASSERT_TRUE(edit.success) << edit.output;

    ToolResult after_edit = read_tool.execute(nlohmann::json({{"file_path", p.string()}}).dump(),
                                              ToolContext{});
    ASSERT_TRUE(after_edit.success) << after_edit.output;
    EXPECT_NE(after_edit.output.find("alpha\ndelta\n"), std::string::npos);
    EXPECT_EQ(after_edit.output.find("File unchanged since last read"), std::string::npos);

    ToolResult repeated = read_tool.execute(nlohmann::json({{"file_path", p.string()}}).dump(),
                                            ToolContext{});
    ASSERT_TRUE(repeated.success) << repeated.output;
    EXPECT_NE(repeated.output.find("File unchanged since last read"), std::string::npos);
    EXPECT_EQ(repeated.output.find("alpha\ndelta\n"), std::string::npos);

    fs::remove(p);
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
