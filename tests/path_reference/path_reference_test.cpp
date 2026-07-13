#include <gtest/gtest.h>

#include "path_reference/path_reference.hpp"
#include "utils/encoding.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
namespace pr = acecode::path_reference;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() /
            ("acecode_path_reference_" +
             std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

fs::path utf8_path(const std::string& value) {
#ifdef _WIN32
    return fs::path(acecode::utf8_to_wide(value));
#else
    return fs::path(value);
#endif
}

std::string utf8_string(const fs::path& value) {
#ifdef _WIN32
    return acecode::wide_to_utf8(value.generic_wstring());
#else
    return value.generic_string();
#endif
}

void touch(const fs::path& path) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << "x";
}

} // namespace

TEST(PathReference, FindsBareTokenAtCursor) {
    auto token = pr::token_at_cursor("please @src/ma continue", 14);
    ASSERT_TRUE(token.has_value());
    EXPECT_EQ(token->begin, 7u);
    EXPECT_EQ(token->end, 14u);
    EXPECT_EQ(token->path, "src/ma");
    EXPECT_FALSE(token->quoted);
}

TEST(PathReference, ReplacesCompleteMiddleToken) {
    const std::string input = "please @src/ma continue";
    auto token = pr::token_at_cursor(input, 12);
    ASSERT_TRUE(token.has_value());
    auto replaced = pr::replace_token(input, *token, "src/main.cpp", false, false);
    EXPECT_EQ(replaced.text, "please @src/main.cpp  continue");
    EXPECT_EQ(replaced.cursor, std::string("please @src/main.cpp ").size());
}

TEST(PathReference, SupportsQuotedUnicodeAndWhitespace) {
    const std::string input = u8"看 @\"文档/设计 草案\" 后继续";
    const auto cursor = input.find(u8"草案") + std::string(u8"草案").size();
    auto token = pr::token_at_cursor(input, cursor);
    ASSERT_TRUE(token.has_value());
    EXPECT_TRUE(token->quoted);
    EXPECT_EQ(token->path, u8"文档/设计 草案");
    EXPECT_EQ(pr::format_reference(u8"文档/设计 草案.md", false),
              u8"@\"文档/设计 草案.md\" ");
}

TEST(PathReference, IgnoresEmailAndRequiresTokenBoundary) {
    EXPECT_FALSE(pr::token_at_cursor("mail user@example.com", 17).has_value());
    EXPECT_FALSE(pr::token_at_cursor("prefixX@src", 11).has_value());
}

TEST(PathReference, SplitsBothSeparatorStyles) {
    auto forward = pr::split_query("src/deep/ma");
    EXPECT_EQ(forward.directory, "src/deep");
    EXPECT_EQ(forward.filter, "ma");
    auto backward = pr::split_query("src\\deep\\ma");
    EXPECT_EQ(backward.directory, "src/deep");
    EXPECT_EQ(backward.filter, "ma");
}

TEST(PathReference, FormatsAndEntersDirectories) {
    EXPECT_EQ(pr::format_reference("src", true), "@src/ ");
    const auto token = *pr::token_at_cursor("@sr", 3);
    auto entered = pr::replace_token("@sr", token, "src", true, true);
    EXPECT_EQ(entered.text, "@src/");
    EXPECT_EQ(entered.cursor, 5u);
}

TEST(PathReference, SuggestsOneLevelIncludingHiddenAndNoise) {
    TempDir tmp;
    fs::create_directories(tmp.path / ".git");
    fs::create_directories(tmp.path / "src");
    touch(tmp.path / ".env");
    touch(tmp.path / "README.md");

    auto result = pr::suggest(utf8_string(tmp.path), "");
    ASSERT_TRUE(result.error.empty()) << result.error;
    ASSERT_EQ(result.items.size(), 4u);
    EXPECT_EQ(result.items[0].name, ".git");
    EXPECT_EQ(result.items[1].name, "src");
    EXPECT_TRUE(result.items[0].is_directory);
    EXPECT_FALSE(result.items[2].is_directory);
}

TEST(PathReference, SuggestsNestedDirectoryAndFiltersCaseInsensitively) {
    TempDir tmp;
    fs::create_directories(tmp.path / "src");
    touch(tmp.path / "src" / "Main.cpp");
    touch(tmp.path / "src" / "other.cpp");
    auto result = pr::suggest(utf8_string(tmp.path), "src/ma");
    ASSERT_TRUE(result.error.empty()) << result.error;
    ASSERT_EQ(result.items.size(), 1u);
    EXPECT_EQ(result.items[0].path, "src/Main.cpp");
}

TEST(PathReference, SuggestionLimitIsFifty) {
    TempDir tmp;
    for (int i = 0; i < 70; ++i) {
        touch(tmp.path / ("file_" + std::to_string(i) + ".txt"));
    }
    auto result = pr::suggest(utf8_string(tmp.path), "file");
    ASSERT_TRUE(result.error.empty()) << result.error;
    EXPECT_EQ(result.items.size(), pr::kMaxSuggestions);
}

TEST(PathReference, RejectsTraversalAndAbsolutePaths) {
    TempDir tmp;
    EXPECT_FALSE(pr::suggest(utf8_string(tmp.path), "../secret").error.empty());
    EXPECT_FALSE(pr::suggest(utf8_string(tmp.path), "/etc").error.empty());
    EXPECT_FALSE(pr::suggest(utf8_string(tmp.path), "C:\\Windows").error.empty());
}

TEST(PathReference, SkipsDirectoryLinksThatCouldEscapeCwd) {
    TempDir cwd;
    TempDir outside;
    fs::create_directories(outside.path / "secret");
    std::error_code ec;
    fs::create_directory_symlink(outside.path / "secret", cwd.path / "escape", ec);
    if (ec) GTEST_SKIP() << ec.message();
    auto result = pr::suggest(utf8_string(cwd.path), "");
    ASSERT_TRUE(result.error.empty()) << result.error;
    EXPECT_TRUE(std::none_of(result.items.begin(), result.items.end(), [](const auto& item) {
        return item.name == "escape";
    }));
}

TEST(PathReference, MissingDirectoryReturnsNonBlockingError) {
    TempDir tmp;
    auto result = pr::suggest(utf8_string(tmp.path), "gone/");
    EXPECT_TRUE(result.items.empty());
    EXPECT_FALSE(result.error.empty());
}

TEST(PathReference, UnicodeDirectoryRoundTripsAsUtf8) {
    TempDir tmp;
    fs::create_directories(tmp.path / utf8_path(u8"中文目录"));
    touch(tmp.path / utf8_path(u8"中文文件.txt"));
    auto result = pr::suggest(utf8_string(tmp.path), u8"中文");
    ASSERT_TRUE(result.error.empty()) << result.error;
    ASSERT_EQ(result.items.size(), 2u);
    EXPECT_EQ(result.items[0].name, u8"中文目录");
    EXPECT_EQ(result.items[1].name, u8"中文文件.txt");
}
