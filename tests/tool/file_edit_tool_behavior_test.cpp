#include <gtest/gtest.h>

#include "tool/file_edit_tool.hpp"
#include "tool/file_read_tool.hpp"
#include "tool/mtime_tracker.hpp"
#include "tool/tool_executor.hpp"

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

TEST(FileEditToolBehavior, RejectsExistingFileWithoutFullRead) {
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

TEST(FileEditToolBehavior, RejectsPartialReadBeforeEdit) {
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

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.output.find("only partially read"), std::string::npos);
    EXPECT_EQ(read_file(path), "alpha\nbeta\ngamma\n");

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
