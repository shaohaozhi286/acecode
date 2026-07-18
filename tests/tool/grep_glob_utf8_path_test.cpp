#include <gtest/gtest.h>

#include "tool/glob_tool.hpp"
#include "tool/grep_tool.hpp"
#include "utils/encoding.hpp"
#include "utils/utf8_path.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <random>

namespace fs = std::filesystem;

namespace {

struct TempTree {
    fs::path path;

    TempTree() {
        path = fs::temp_directory_path() /
               ("acecode_grep_glob_utf8_" + std::to_string(std::random_device{}()));
        fs::create_directories(path);
    }

    ~TempTree() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream ofs(path, std::ios::binary);
    ofs << content;
}

} // namespace

TEST(GrepGlobUtf8PathTest, GlobReturnsUtf8RelativePaths) {
    TempTree tmp;
    fs::path root = tmp.path / acecode::path_from_utf8(u8"项目");
    write_file(root / acecode::path_from_utf8(u8"子目录/匹配.txt"), "content\n");

    acecode::ToolContext ctx;
    ctx.cwd = acecode::path_to_utf8(root);

    auto tool = acecode::create_glob_tool();
    auto result = tool.execute(R"({"pattern":"**/*.txt"})", ctx);

    ASSERT_TRUE(result.success) << result.output;
    EXPECT_NE(result.output.find(u8"子目录/匹配.txt"), std::string::npos);
    EXPECT_TRUE(acecode::is_valid_utf8(result.output));
}

TEST(GrepGlobUtf8PathTest, GrepReturnsUtf8PathsAndLines) {
    TempTree tmp;
    fs::path root = tmp.path / acecode::path_from_utf8(u8"项目");
    write_file(root / acecode::path_from_utf8(u8"资料/笔记.txt"), u8"第一行\n关键词在这里\n");

    acecode::ToolContext ctx;
    ctx.cwd = acecode::path_to_utf8(root);

    auto tool = acecode::create_grep_tool();
    auto result = tool.execute(R"({"pattern":"关键词","include_pattern":"*.txt"})", ctx);

    ASSERT_TRUE(result.success) << result.output;
    EXPECT_NE(result.output.find(u8"资料/笔记.txt"), std::string::npos);
    EXPECT_NE(result.output.find(u8"关键词在这里"), std::string::npos);
    EXPECT_TRUE(acecode::is_valid_utf8(result.output));
}

TEST(GrepGlobUtf8PathTest, GrepAcceptsFilePath) {
    TempTree tmp;
    fs::path root = tmp.path / "project";
    write_file(root / "CMakeLists.txt", "add_executable(acecode src/main.cpp)\n");
    write_file(root / "other.txt", "src/main.cpp\n");

    acecode::ToolContext ctx;
    ctx.cwd = acecode::path_to_utf8(root);

    auto args = nlohmann::json({
        {"pattern", "main\\.cpp"},
        {"path", acecode::path_to_utf8(root / "CMakeLists.txt")},
    }).dump();

    auto tool = acecode::create_grep_tool();
    auto result = tool.execute(args, ctx);

    ASSERT_TRUE(result.success) << result.output;
    EXPECT_NE(result.output.find("CMakeLists.txt:1:add_executable"), std::string::npos);
    EXPECT_EQ(result.output.find("other.txt"), std::string::npos);
}

TEST(GrepGlobUtf8PathTest, GrepSearchesExplicitFileLargerThanOneMiB) {
    TempTree tmp;
    fs::path file = tmp.path / "large.log";
    {
        std::ofstream ofs(file, std::ios::binary);
        const std::string padding(100, 'x');
        for (int line = 1; line <= 11000; ++line) {
            ofs << (line == 10999 ? "large-file-needle " : "ordinary ")
                << padding << "\n";
        }
    }
    ASSERT_GT(fs::file_size(file), 1024u * 1024u);

    auto tool = acecode::create_grep_tool();
    auto result = tool.execute(nlohmann::json({
        {"pattern", "large-file-needle"},
        {"path", acecode::path_to_utf8(file)}
    }).dump(), acecode::ToolContext{});

    ASSERT_TRUE(result.success) << result.output;
    EXPECT_NE(result.output.find("large.log:10999:large-file-needle"),
              std::string::npos);
}

TEST(GrepGlobUtf8PathTest, RecursiveGrepDoesNotSkipLargeFiles) {
    TempTree tmp;
    fs::path root = tmp.path / "project";
    fs::path file = root / "logs" / "large.log";
    fs::create_directories(file.parent_path());
    {
        std::ofstream ofs(file, std::ios::binary);
        ofs << std::string(1024 * 1024 + 128, 'a') << "\n";
        ofs << "recursive-large-file-needle\n";
    }

    acecode::ToolContext ctx;
    ctx.cwd = acecode::path_to_utf8(root);
    auto tool = acecode::create_grep_tool();
    auto result = tool.execute(
        R"({"pattern":"recursive-large-file-needle","include_pattern":"*.log"})",
        ctx);

    ASSERT_TRUE(result.success) << result.output;
    EXPECT_NE(result.output.find("logs/large.log:2:recursive-large-file-needle"),
              std::string::npos);
}

TEST(GrepGlobUtf8PathTest, GrepBoundsLongLinesAndAggregateOutput) {
    TempTree tmp;
    fs::path file = tmp.path / "many-matches.log";
    {
        std::ofstream ofs(file, std::ios::binary);
        for (int line = 0; line < 100; ++line) {
            ofs << "needle-" << line << "-" << std::string(5000, 'z') << "\n";
        }
    }

    auto tool = acecode::create_grep_tool();
    auto result = tool.execute(nlohmann::json({
        {"pattern", "needle"},
        {"path", acecode::path_to_utf8(file)}
    }).dump(), acecode::ToolContext{});

    ASSERT_TRUE(result.success) << result.output;
    EXPECT_NE(result.output.find("[line shortened]"), std::string::npos);
    EXPECT_NE(result.output.find("48KiB output limit"), std::string::npos);
    EXPECT_LE(result.output.size(), 48u * 1024u);
    EXPECT_TRUE(acecode::is_valid_utf8(result.output));
}
