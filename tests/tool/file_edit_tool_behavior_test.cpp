#include <gtest/gtest.h>

#include "tool/file_edit_tool.hpp"
#include "tool/file_read_tool.hpp"
#include "tool/mtime_tracker.hpp"
#include "tool/tool_executor.hpp"
#include "utils/text_file_buffer.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

using acecode::ToolContext;
using acecode::ToolImpl;
using acecode::ToolResult;
using acecode::create_file_edit_tool;
using acecode::create_file_read_tool;

namespace {

fs::path temp_file(const std::string& suffix) {
    static std::atomic<int> seq{0};
    auto path = fs::temp_directory_path() /
                ("acecode_file_edit_behavior_" + std::to_string(++seq) + suffix);
    fs::remove(path);
    return path;
}

void write_file(const fs::path& path, const std::string& content) {
    std::ofstream ofs(path, std::ios::binary);
    ofs << content;
}

std::string read_file(const fs::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(ifs)),
                       std::istreambuf_iterator<char>());
}

void mark_full_read(const fs::path& path) {
    acecode::MtimeTracker::instance().record_read(path.string(), read_file(path), false);
}

ToolResult run_edit(const nlohmann::json& args) {
    ToolImpl tool = create_file_edit_tool();
    return tool.execute(args.dump(), ToolContext{});
}

} // namespace

TEST(FileEditToolBehavior, RejectsExistingFileWithoutReadBaseline) {
    auto path = temp_file(".txt");
    write_file(path, "alpha\nbeta\n");

    ToolResult result = run_edit({
        {"file_path", path.string()},
        {"old_string", "beta"},
        {"new_string", "gamma"}
    });

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.output.find("not been read"), std::string::npos);
    EXPECT_EQ(read_file(path), "alpha\nbeta\n");

    fs::remove(path);
}

TEST(FileEditToolBehavior, AllowsPartialReadBaselineBeforeOldStringEdit) {
    auto path = temp_file(".txt");
    write_file(path, "alpha\nbeta\ngamma\n");

    ToolImpl read_tool = create_file_read_tool();
    ToolResult read_result = read_tool.execute(nlohmann::json({
        {"file_path", path.string()},
        {"start_line", 1},
        {"end_line", 1}
    }).dump(), ToolContext{});
    ASSERT_TRUE(read_result.success);

    ToolResult result = run_edit({
        {"file_path", path.string()},
        {"old_string", "beta"},
        {"new_string", "delta"}
    });

    ASSERT_TRUE(result.success) << result.output;
    EXPECT_EQ(read_file(path), "alpha\ndelta\ngamma\n");

    fs::remove(path);
}

TEST(FileEditToolBehavior, RejectsStalePartialReadBeforeOldStringEdit) {
    auto path = temp_file(".txt");
    write_file(path, "alpha\nbeta\ngamma\n");

    ToolImpl read_tool = create_file_read_tool();
    ToolResult read_result = read_tool.execute(nlohmann::json({
        {"file_path", path.string()},
        {"start_line", 1},
        {"end_line", 1}
    }).dump(), ToolContext{});
    ASSERT_TRUE(read_result.success);

    auto mtime = fs::last_write_time(path);
    write_file(path, "alpha\nchanged\ngamma\n");
    fs::last_write_time(path, mtime + std::chrono::seconds(3));

    ToolResult result = run_edit({
        {"file_path", path.string()},
        {"old_string", "changed"},
        {"new_string", "delta"}
    });

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.output.find("modified externally"), std::string::npos);
    EXPECT_EQ(read_file(path), "alpha\nchanged\ngamma\n");

    fs::remove(path);
}

TEST(FileEditToolBehavior, ReplaceAllReplacesEveryOccurrence) {
    auto path = temp_file(".txt");
    write_file(path, "foo foo\nfoo\n");
    mark_full_read(path);

    ToolResult result = run_edit({
        {"file_path", path.string()},
        {"old_string", "foo"},
        {"new_string", "bar"},
        {"replace_all", true}
    });

    ASSERT_TRUE(result.success);
    EXPECT_EQ(read_file(path), "bar bar\nbar\n");

    fs::remove(path);
}

TEST(FileEditToolBehavior, ReplaceAllAcceptsSemanticStringTrue) {
    auto path = temp_file(".txt");
    write_file(path, "foo foo\n");
    mark_full_read(path);

    ToolResult result = run_edit({
        {"file_path", path.string()},
        {"old_string", "foo"},
        {"new_string", "bar"},
        {"replace_all", "true"}
    });

    ASSERT_TRUE(result.success);
    EXPECT_EQ(read_file(path), "bar bar\n");

    fs::remove(path);
}

TEST(FileEditToolBehavior, MultipleMatchesRequireReplaceAllOrContext) {
    auto path = temp_file(".txt");
    write_file(path, "foo foo\n");
    mark_full_read(path);

    ToolResult result = run_edit({
        {"file_path", path.string()},
        {"old_string", "foo"},
        {"new_string", "bar"}
    });

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.output.find("replace_all"), std::string::npos);
    EXPECT_EQ(read_file(path), "foo foo\n");

    fs::remove(path);
}

TEST(FileEditToolBehavior, AllowsTimestampOnlyChangeWhenContentUnchanged) {
    auto path = temp_file(".txt");
    write_file(path, "alpha\nbeta\n");
    mark_full_read(path);

    auto mtime = fs::last_write_time(path);
    fs::last_write_time(path, mtime + std::chrono::seconds(3));

    ToolResult result = run_edit({
        {"file_path", path.string()},
        {"old_string", "beta"},
        {"new_string", "gamma"}
    });

    ASSERT_TRUE(result.success);
    EXPECT_EQ(read_file(path), "alpha\ngamma\n");

    fs::remove(path);
}

TEST(FileEditToolBehavior, RejectsExternalContentChangeAfterRead) {
    auto path = temp_file(".txt");
    write_file(path, "alpha\nbeta\n");
    mark_full_read(path);

    auto mtime = fs::last_write_time(path);
    write_file(path, "alpha\nchanged\n");
    fs::last_write_time(path, mtime + std::chrono::seconds(3));

    ToolResult result = run_edit({
        {"file_path", path.string()},
        {"old_string", "changed"},
        {"new_string", "gamma"}
    });

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.output.find("modified externally"), std::string::npos);
    EXPECT_EQ(read_file(path), "alpha\nchanged\n");

    fs::remove(path);
}

TEST(FileEditToolBehavior, EmptyOldStringCreatesMissingFile) {
    auto path = temp_file(".txt");

    ToolResult result = run_edit({
        {"file_path", path.string()},
        {"old_string", ""},
        {"new_string", "created\n"}
    });

    ASSERT_TRUE(result.success);
    ASSERT_TRUE(result.summary.has_value());
    EXPECT_EQ(result.summary->verb, "Created");
    EXPECT_EQ(read_file(path), "created\n");

    fs::remove(path);
}

TEST(FileEditToolBehavior, EmptyOldStringRejectsExistingNonBlankFile) {
    auto path = temp_file(".txt");
    write_file(path, "already here\n");

    ToolResult result = run_edit({
        {"file_path", path.string()},
        {"old_string", ""},
        {"new_string", "created\n"}
    });

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.output.find("already exists"), std::string::npos);
    EXPECT_EQ(read_file(path), "already here\n");

    fs::remove(path);
}

TEST(FileEditToolBehavior, EmptyOldStringReplacesBlankFile) {
    auto path = temp_file(".txt");
    write_file(path, " \n\t");

    ToolResult result = run_edit({
        {"file_path", path.string()},
        {"old_string", ""},
        {"new_string", "filled\n"}
    });

    ASSERT_TRUE(result.success);
    EXPECT_EQ(read_file(path), "filled\n");

    fs::remove(path);
}

TEST(FileEditToolBehavior, RejectsNoopEdit) {
    auto path = temp_file(".txt");
    write_file(path, "same\n");

    ToolResult result = run_edit({
        {"file_path", path.string()},
        {"old_string", "same"},
        {"new_string", "same"}
    });

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.output.find("No changes"), std::string::npos);
    EXPECT_EQ(read_file(path), "same\n");

    fs::remove(path);
}

TEST(FileEditToolBehavior, RejectsNotebookFiles) {
    auto path = temp_file(".ipynb");
    write_file(path, "{}\n");

    ToolResult result = run_edit({
        {"file_path", path.string()},
        {"old_string", "{}"},
        {"new_string", "{\"cells\":[]}"}
    });

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.output.find("Jupyter Notebook"), std::string::npos);
    EXPECT_EQ(read_file(path), "{}\n");

    fs::remove(path);
}

TEST(FileEditToolBehavior, LfAnchorMatchesCrLfFileAndPreservesCrLf) {
    auto path = temp_file(".txt");
    write_file(path, "a\r\nold\r\nb\r\n");
    mark_full_read(path);

    ToolResult result = run_edit({
        {"file_path", path.string()},
        {"old_string", "a\nold\nb\n"},
        {"new_string", "a\nnew\nb\n"}
    });

    ASSERT_TRUE(result.success);
    EXPECT_EQ(read_file(path), "a\r\nnew\r\nb\r\n");

    fs::remove(path);
}

TEST(FileEditToolBehavior, AsciiQuoteAnchorMatchesCurlyQuoteFile) {
    auto path = temp_file(".txt");
    write_file(path, u8"const title = “Hello”;\n");
    mark_full_read(path);

    ToolResult result = run_edit({
        {"file_path", path.string()},
        {"old_string", "const title = \"Hello\";"},
        {"new_string", "const title = \"Hi\";"}
    });

    ASSERT_TRUE(result.success);
    EXPECT_EQ(read_file(path), std::string(u8"const title = “Hi”;\n"));

    fs::remove(path);
}

TEST(FileEditToolBehavior, RangeEditUsesHashAndPreservesCrLf) {
    auto path = temp_file(".txt");
    write_file(path, "a\r\nb\r\nc\r\n");
    auto decoded = acecode::read_text_file_buffer(path.string());
    ASSERT_TRUE(decoded.success) << decoded.error;
    std::string hash = acecode::range_hash(decoded.buffer.text, 2, 2);

    ToolResult result = run_edit({
        {"file_path", path.string()},
        {"start_line", 2},
        {"end_line", 2},
        {"expected_hash", hash},
        {"new_string", "B\n"}
    });

    ASSERT_TRUE(result.success) << result.output;
    EXPECT_EQ(read_file(path), "a\r\nB\r\nc\r\n");

    fs::remove(path);
}

TEST(FileEditToolBehavior, RangeEditHashMismatchReturnsCurrentRange) {
    auto path = temp_file(".txt");
    write_file(path, "a\nb\nc\n");

    ToolResult result = run_edit({
        {"file_path", path.string()},
        {"start_line", 2},
        {"end_line", 2},
        {"expected_hash", "sha256:wrong"},
        {"new_string", "B\n"}
    });

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.output.find("range hash mismatch"), std::string::npos);
    EXPECT_NE(result.output.find("b\n"), std::string::npos);
    EXPECT_EQ(read_file(path), "a\nb\nc\n");

    fs::remove(path);
}

TEST(FileEditToolBehavior, RangeEditAllowsRedundantOldStringWhenHashMatches) {
    auto path = temp_file(".txt");
    write_file(path, "a\nb\nc\n");
    auto decoded = acecode::read_text_file_buffer(path.string());
    ASSERT_TRUE(decoded.success) << decoded.error;
    std::string hash = acecode::range_hash(decoded.buffer.text, 2, 2);

    ToolResult result = run_edit({
        {"file_path", path.string()},
        {"start_line", 2},
        {"end_line", 2},
        {"expected_hash", hash},
        {"old_string", "b"},
        {"new_string", "B"}
    });

    ASSERT_TRUE(result.success) << result.output;
    EXPECT_EQ(read_file(path), "a\nB\nc\n");

    fs::remove(path);
}

TEST(FileEditToolBehavior, RangeEditStaleHashCanUseOldStringAsCurrentRangeGuard) {
    auto path = temp_file(".txt");
    write_file(path, "a\nb\nc\n");

    ToolResult result = run_edit({
        {"file_path", path.string()},
        {"start_line", 2},
        {"end_line", 2},
        {"expected_hash", "sha256:stale"},
        {"old_string", "b"},
        {"new_string", "B"}
    });

    ASSERT_TRUE(result.success) << result.output;
    EXPECT_EQ(read_file(path), "a\nB\nc\n");

    fs::remove(path);
}

TEST(FileEditToolBehavior, RangeEditStaleHashAlreadyAppliedSucceedsWithoutWrite) {
    auto path = temp_file(".txt");
    write_file(path, "a\nB\nc\n");

    ToolResult result = run_edit({
        {"file_path", path.string()},
        {"start_line", 2},
        {"end_line", 2},
        {"expected_hash", "sha256:stale"},
        {"new_string", "B"}
    });

    ASSERT_TRUE(result.success) << result.output;
    EXPECT_NE(result.output.find("already applied"), std::string::npos);
    ASSERT_TRUE(result.summary.has_value());
    EXPECT_EQ(result.summary->verb, "Already applied");
    EXPECT_EQ(read_file(path), "a\nB\nc\n");

    fs::remove(path);
}

TEST(FileEditToolBehavior, RangeEditStaleHashStillRejectsNonMatchingOldString) {
    auto path = temp_file(".txt");
    write_file(path, "a\nb\nc\n");

    ToolResult result = run_edit({
        {"file_path", path.string()},
        {"start_line", 2},
        {"end_line", 2},
        {"expected_hash", "sha256:stale"},
        {"old_string", "different"},
        {"new_string", "B"}
    });

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.output.find("range hash mismatch"), std::string::npos);
    EXPECT_EQ(read_file(path), "a\nb\nc\n");

    fs::remove(path);
}

// 回归测试:range 模式的删除区间包含 end_line 行尾的换行符,而模型给出的 new_string
// 习惯上不带尾随换行。修复前替换后 end_line 的下一行会被直接拼接到新内容末尾,
// 实际损伤如 `});` 与 `const handleImageLoaded = (ev) => {` 粘连成一行,
// 模型随后反复修粘连、修的过程中再次触发同一 bug,形成编辑死循环。
// 期望:工具自动补回被删除区间携带的尾随换行,下一行保持独立。
TEST(FileEditToolBehavior, RangeEditWithoutTrailingNewlineDoesNotGlueNextLine) {
    auto path = temp_file(".txt");
    write_file(path, "const a = 1;\n});\n\nconst next = 2;\n");
    auto decoded = acecode::read_text_file_buffer(path.string());
    ASSERT_TRUE(decoded.success) << decoded.error;
    // 替换 1-3 行(含末尾空行),new_string 刻意不带尾随换行,模拟模型的常见输出
    std::string hash = acecode::range_hash(decoded.buffer.text, 1, 3);

    ToolResult result = run_edit({
        {"file_path", path.string()},
        {"start_line", 1},
        {"end_line", 3},
        {"expected_hash", hash},
        {"new_string", "const a = 42;\n});"}
    });

    ASSERT_TRUE(result.success) << result.output;
    EXPECT_EQ(read_file(path), "const a = 42;\n});\nconst next = 2;\n");

    fs::remove(path);
}

// 场景:new_string 为空表示删除整个行区间。期望:被删行连同其换行符一起移除,
// 不能因为补换行逻辑而留下一个多余的空行。
TEST(FileEditToolBehavior, RangeEditEmptyNewStringDeletesWholeLines) {
    auto path = temp_file(".txt");
    write_file(path, "a\nb\nc\n");
    auto decoded = acecode::read_text_file_buffer(path.string());
    ASSERT_TRUE(decoded.success) << decoded.error;
    std::string hash = acecode::range_hash(decoded.buffer.text, 2, 2);

    ToolResult result = run_edit({
        {"file_path", path.string()},
        {"start_line", 2},
        {"end_line", 2},
        {"expected_hash", hash},
        {"new_string", ""}
    });

    ASSERT_TRUE(result.success) << result.output;
    EXPECT_EQ(read_file(path), "a\nc\n");

    fs::remove(path);
}

// 场景:替换的 end_line 是文件最后一行且文件本身没有尾随换行。
// 期望:删除区间不含换行符,补换行逻辑不触发,结果同样保持无尾随换行。
TEST(FileEditToolBehavior, RangeEditAtEofWithoutTrailingNewlineAddsNothing) {
    auto path = temp_file(".txt");
    write_file(path, "a\nb");
    auto decoded = acecode::read_text_file_buffer(path.string());
    ASSERT_TRUE(decoded.success) << decoded.error;
    std::string hash = acecode::range_hash(decoded.buffer.text, 2, 2);

    ToolResult result = run_edit({
        {"file_path", path.string()},
        {"start_line", 2},
        {"end_line", 2},
        {"expected_hash", hash},
        {"new_string", "B"}
    });

    ASSERT_TRUE(result.success) << result.output;
    EXPECT_EQ(read_file(path), "a\nB");

    fs::remove(path);
}

TEST(FileEditToolBehavior, LossyReadDoesNotEnableLaterEdit) {
    auto path = temp_file(".txt");
    std::string original = std::string(u8"中文\n");
    original.push_back(static_cast<char>(0xE4));
    write_file(path, original);

    ToolImpl read_tool = create_file_read_tool();
    ToolResult read_result = read_tool.execute(
        nlohmann::json({{"file_path", path.string()}}).dump(), ToolContext{});
    ASSERT_TRUE(read_result.success) << read_result.output;
    EXPECT_NE(read_result.output.find("lossy=\"true\""), std::string::npos);
    EXPECT_EQ(read_result.output.find("range_hash=\"sha256:"), std::string::npos);

    auto read_check = acecode::MtimeTracker::instance().validate_read_baseline_for_edit(
        path.string(), std::string(u8"中文\n") + std::string("\xEF\xBF\xBD", 3));
    EXPECT_EQ(read_check.status, acecode::MtimeTracker::ReadBaselineStatus::UnsafeRead);

    ToolResult result = run_edit({
        {"file_path", path.string()},
        {"old_string", std::string(u8"中文")},
        {"new_string", std::string(u8"改动")}
    });

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.output.find("not safe enough"), std::string::npos);
    EXPECT_EQ(read_file(path), original);

    fs::remove(path);
}

#ifdef _WIN32
TEST(FileEditToolBehavior, GbkFileEditPreservesLegacyEncoding) {
    auto path = temp_file(".txt");
    write_file(path, std::string("\xD6\xD0\xCE\xC4\n", 5)); // "中文\n" in CP936
    mark_full_read(path);

    ToolResult result = run_edit({
        {"file_path", path.string()},
        {"old_string", std::string(u8"中文")},
        {"new_string", std::string(u8"改动")}
    });

    ASSERT_TRUE(result.success) << result.output;
    std::string raw = read_file(path);
    EXPECT_EQ(raw.find(std::string(u8"改动")), std::string::npos);
    auto decoded = acecode::read_text_file_buffer(path.string());
    ASSERT_TRUE(decoded.success) << decoded.error;
    EXPECT_EQ(decoded.buffer.text, std::string(u8"改动\n"));
}

TEST(FileEditToolBehavior, GbkUnrepresentableReplacementFailsBeforeMutation) {
    auto path = temp_file(".txt");
    std::string original("\xD6\xD0\xCE\xC4\n", 5); // "中文\n" in CP936
    write_file(path, original);
    mark_full_read(path);

    ToolResult result = run_edit({
        {"file_path", path.string()},
        {"old_string", std::string(u8"中文")},
        {"new_string", std::string(u8"emoji \xF0\x9F\x98\x80")}
    });

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.output.find("target encoding"), std::string::npos);
    EXPECT_EQ(read_file(path), original);

    fs::remove(path);
}
#endif
