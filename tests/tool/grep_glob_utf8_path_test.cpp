#include <gtest/gtest.h>

#include "tool/glob_tool.hpp"
#include "tool/grep_tool.hpp"
#include "utils/encoding.hpp"
#include "utils/utf8_path.hpp"

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
