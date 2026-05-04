// 覆盖 src/web/handlers/files_handler.cpp 的纯函数:
//   - validate_path_within: cwd 白名单 + 越权防御(.. / 绝对 / Windows 反斜杠 /
//     符号链接)
//   - list_directory: 隐藏文件过滤、noise 黑名单、kind=dir 排序优先
//   - read_file_content: 5MB cap、二进制嗅探、UTF-8 文本读取、不存在文件
//
// 测试均在 tmp dir 下,RAII 清理(TempDir 析构 → fs::remove_all)。同 process 内
// 不与其它测试共享路径,跨平台一致(Windows / POSIX)。

#include <gtest/gtest.h>

#include "web/handlers/files_handler.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using acecode::web::FileEntry;
using acecode::web::FileError;
using acecode::web::FileErrorKind;
using acecode::web::list_directory;
using acecode::web::read_file_content;
using acecode::web::validate_path_within;

namespace {

// RAII 临时目录:构造时建,析构时 remove_all。失败 swallow(unit test 环境).
struct TempDir {
    fs::path path;
    TempDir() {
        auto base = fs::temp_directory_path();
        // 用 PID + 时间避免并发测试撞名
        auto stamp = std::to_string(reinterpret_cast<std::uintptr_t>(this));
        path = base / ("acecode_files_test_" + stamp);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream ofs(p, std::ios::binary);
    ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::string cwd_str(const TempDir& d) {
    return d.path.lexically_normal().generic_string();
}

} // namespace

// 场景:cwd 不在 allowed_cwds 列表 → UnknownWorkspace。
TEST(FilesHandler, PathValidatorRejectsUnknownCwd) {
    TempDir tmp;
    auto result = validate_path_within(cwd_str(tmp), "", {"/some/other/path"});
    ASSERT_TRUE(std::holds_alternative<FileError>(result));
    EXPECT_EQ(std::get<FileError>(result).kind, FileErrorKind::UnknownWorkspace);
}

// 场景:cwd 在白名单 + path 用 `..` 跳出 → PathOutsideWorkspace。
TEST(FilesHandler, PathValidatorRejectsParentTraversal) {
    TempDir tmp;
    auto cwd = cwd_str(tmp);
    auto result = validate_path_within(cwd, "../../../../etc", {cwd});
    ASSERT_TRUE(std::holds_alternative<FileError>(result));
    EXPECT_EQ(std::get<FileError>(result).kind, FileErrorKind::PathOutsideWorkspace);
}

// 场景:path 用绝对路径跳出 cwd 子树 → PathOutsideWorkspace。
TEST(FilesHandler, PathValidatorRejectsAbsoluteEscape) {
    TempDir tmp;
    auto cwd = cwd_str(tmp);
#ifdef _WIN32
    auto outside = std::string("C:/Windows");
#else
    auto outside = std::string("/etc/passwd");
#endif
    auto result = validate_path_within(cwd, outside, {cwd});
    ASSERT_TRUE(std::holds_alternative<FileError>(result));
    EXPECT_EQ(std::get<FileError>(result).kind, FileErrorKind::PathOutsideWorkspace);
}

// 场景:合法子路径 → 返回解析后的绝对 path。
TEST(FilesHandler, PathValidatorAcceptsValidSubpath) {
    TempDir tmp;
    fs::create_directories(tmp.path / "src" / "deep");
    auto cwd = cwd_str(tmp);
    auto result = validate_path_within(cwd, "src/deep", {cwd});
    ASSERT_TRUE(std::holds_alternative<fs::path>(result));
    auto p = std::get<fs::path>(result);
    EXPECT_TRUE(fs::equivalent(p, tmp.path / "src" / "deep"));
}

// 场景:空 path = cwd 根本身,合法。
TEST(FilesHandler, PathValidatorAcceptsEmptyPath) {
    TempDir tmp;
    auto cwd = cwd_str(tmp);
    auto result = validate_path_within(cwd, "", {cwd});
    ASSERT_TRUE(std::holds_alternative<fs::path>(result));
}

// 场景:列出目录默认过滤 dot 开头的隐藏文件。
TEST(FilesHandler, ListDirectoryFiltersHiddenByDefault) {
    TempDir tmp;
    write_file(tmp.path / ".env", "secret");
    write_file(tmp.path / "README.md", "# hi");
    auto result = list_directory(tmp.path, tmp.path, /*show_hidden=*/false);
    ASSERT_TRUE(std::holds_alternative<std::vector<FileEntry>>(result));
    auto& entries = std::get<std::vector<FileEntry>>(result);
    bool saw_dot = false;
    bool saw_readme = false;
    for (auto& e : entries) {
        if (e.name == ".env") saw_dot = true;
        if (e.name == "README.md") saw_readme = true;
    }
    EXPECT_FALSE(saw_dot);
    EXPECT_TRUE(saw_readme);
}

// 场景:show_hidden=true 透出 dot 文件,但仍过滤 noise 黑名单(.git)。
TEST(FilesHandler, ListDirectoryRespectsShowHidden) {
    TempDir tmp;
    write_file(tmp.path / ".env", "x");
    fs::create_directories(tmp.path / ".git");
    auto result = list_directory(tmp.path, tmp.path, /*show_hidden=*/true);
    ASSERT_TRUE(std::holds_alternative<std::vector<FileEntry>>(result));
    auto& entries = std::get<std::vector<FileEntry>>(result);
    bool saw_env = false;
    bool saw_git = false;
    for (auto& e : entries) {
        if (e.name == ".env") saw_env = true;
        if (e.name == ".git") saw_git = true;
    }
    EXPECT_TRUE(saw_env);
    EXPECT_FALSE(saw_git);  // noise 黑名单优先级高于 show_hidden
}

// 场景:noise 黑名单始终过滤(node_modules / dist / build / __pycache__)。
TEST(FilesHandler, ListDirectoryAlwaysFiltersNoiseDirs) {
    TempDir tmp;
    fs::create_directories(tmp.path / "node_modules");
    fs::create_directories(tmp.path / "dist");
    fs::create_directories(tmp.path / "build");
    fs::create_directories(tmp.path / "__pycache__");
    fs::create_directories(tmp.path / "src");
    auto result = list_directory(tmp.path, tmp.path, /*show_hidden=*/true);
    ASSERT_TRUE(std::holds_alternative<std::vector<FileEntry>>(result));
    auto& entries = std::get<std::vector<FileEntry>>(result);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].name, "src");
    EXPECT_EQ(entries[0].kind, "dir");
}

// 场景:目录软连接 / mklink 目录不展示,避免前端点击后解析到 workspace 外报 400。
TEST(FilesHandler, ListDirectorySkipsSymlinkDirectories) {
    TempDir tmp;
    TempDir target;
    fs::create_directories(tmp.path / "real_dir");
    fs::create_directories(target.path / "linked_target");

    std::error_code ec;
    fs::create_directory_symlink(target.path / "linked_target", tmp.path / "linked_dir", ec);
    if (ec) {
        GTEST_SKIP() << "directory symlink unsupported in this environment: " << ec.message();
    }

    auto result = list_directory(tmp.path, tmp.path, /*show_hidden=*/true);
    ASSERT_TRUE(std::holds_alternative<std::vector<FileEntry>>(result));
    auto& entries = std::get<std::vector<FileEntry>>(result);
    bool saw_real = false;
    bool saw_link = false;
    for (auto& e : entries) {
        if (e.name == "real_dir") saw_real = true;
        if (e.name == "linked_dir") saw_link = true;
    }
    EXPECT_TRUE(saw_real);
    EXPECT_FALSE(saw_link);
}

// 场景:排序 — 目录优先,然后 name 字典序。
TEST(FilesHandler, ListDirectorySortsDirFirst) {
    TempDir tmp;
    write_file(tmp.path / "z_file.txt", "");
    write_file(tmp.path / "a_file.txt", "");
    fs::create_directories(tmp.path / "z_dir");
    fs::create_directories(tmp.path / "a_dir");
    auto result = list_directory(tmp.path, tmp.path, /*show_hidden=*/false);
    ASSERT_TRUE(std::holds_alternative<std::vector<FileEntry>>(result));
    auto& entries = std::get<std::vector<FileEntry>>(result);
    ASSERT_EQ(entries.size(), 4u);
    EXPECT_EQ(entries[0].name, "a_dir");
    EXPECT_EQ(entries[1].name, "z_dir");
    EXPECT_EQ(entries[2].name, "a_file.txt");
    EXPECT_EQ(entries[3].name, "z_file.txt");
}

// 场景:list_directory 返回的相对路径用 forward-slash(跨平台)。
TEST(FilesHandler, ListDirectoryUsesForwardSlash) {
    TempDir tmp;
    fs::create_directories(tmp.path / "src");
    write_file(tmp.path / "src" / "main.cpp", "int main() {}");
    auto sub = tmp.path / "src";
    auto result = list_directory(sub, tmp.path, /*show_hidden=*/false);
    ASSERT_TRUE(std::holds_alternative<std::vector<FileEntry>>(result));
    auto& entries = std::get<std::vector<FileEntry>>(result);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].path, "src/main.cpp");
}

// 场景:list_directory 目标不存在 → NotFound。
TEST(FilesHandler, ListDirectoryNotFound) {
    TempDir tmp;
    auto missing = tmp.path / "does_not_exist";
    auto result = list_directory(missing, tmp.path, false);
    ASSERT_TRUE(std::holds_alternative<FileError>(result));
    EXPECT_EQ(std::get<FileError>(result).kind, FileErrorKind::NotFound);
}

// 场景:read_file_content 拒绝超过 5MB 的文件,size 字段填实际大小。
TEST(FilesHandler, ReadFileContentRejectsOversize) {
    TempDir tmp;
    auto big = tmp.path / "big.bin";
    constexpr std::uint64_t big_size = 6ull * 1024 * 1024;
    {
        std::ofstream ofs(big, std::ios::binary);
        std::string chunk(1024, 'a');
        for (std::uint64_t i = 0; i < big_size; i += chunk.size()) {
            ofs.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        }
    }
    auto result = read_file_content(big);
    ASSERT_TRUE(std::holds_alternative<FileError>(result));
    auto err = std::get<FileError>(result);
    EXPECT_EQ(err.kind, FileErrorKind::TooLarge);
    EXPECT_GE(err.size, big_size);
}

// 场景:read_file_content 检测到二进制(前 512 字节出现 \0) → Binary。
TEST(FilesHandler, ReadFileContentRejectsBinary) {
    TempDir tmp;
    auto bin = tmp.path / "logo.png";
    std::string content;
    content.push_back('\x89'); content.push_back('P'); content.push_back('N'); content.push_back('G');
    content.push_back('\0');  // \0 触发 binary 嗅探
    content += "rest of binary content";
    write_file(bin, content);
    auto result = read_file_content(bin);
    ASSERT_TRUE(std::holds_alternative<FileError>(result));
    EXPECT_EQ(std::get<FileError>(result).kind, FileErrorKind::Binary);
}

// 场景:UTF-8 文本文件 → 返回完整内容。
TEST(FilesHandler, ReadFileContentReturnsTextUtf8) {
    TempDir tmp;
    auto src = tmp.path / "main.cpp";
    std::string code = "// 你好 world\nint main() { return 0; }\n";
    write_file(src, code);
    auto result = read_file_content(src);
    ASSERT_TRUE(std::holds_alternative<std::string>(result));
    EXPECT_EQ(std::get<std::string>(result), code);
}

// 场景:read_file_content 文件不存在 → NotFound。
TEST(FilesHandler, ReadFileContentRejectsMissingFile) {
    TempDir tmp;
    auto result = read_file_content(tmp.path / "nope.txt");
    ASSERT_TRUE(std::holds_alternative<FileError>(result));
    EXPECT_EQ(std::get<FileError>(result).kind, FileErrorKind::NotFound);
}

// 场景:read_file_content 目录路径 → NotFound(防止误读目录元数据)。
TEST(FilesHandler, ReadFileContentRejectsDirectory) {
    TempDir tmp;
    fs::create_directories(tmp.path / "sub");
    auto result = read_file_content(tmp.path / "sub");
    ASSERT_TRUE(std::holds_alternative<FileError>(result));
    EXPECT_EQ(std::get<FileError>(result).kind, FileErrorKind::NotFound);
}

// 场景:空文件 → 成功返回空字符串(不被误判为二进制)。
TEST(FilesHandler, ReadFileContentEmptyOk) {
    TempDir tmp;
    auto empty = tmp.path / "empty.txt";
    write_file(empty, "");
    auto result = read_file_content(empty);
    ASSERT_TRUE(std::holds_alternative<std::string>(result));
    EXPECT_TRUE(std::get<std::string>(result).empty());
}
