#include "upgrade/package.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <chrono>

namespace fs = std::filesystem;

using namespace acecode::upgrade;

namespace {

fs::path temp_root(const std::string& name) {
    auto p = fs::temp_directory_path() /
             (name + "-" + std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}

void write_file(const fs::path& path, const std::string& body) {
    fs::create_directories(path.parent_path());
    std::ofstream ofs(path, std::ios::binary);
    ofs << body;
}

} // namespace

TEST(UpgradePackage, ValidatesZipEntryPaths) {
    EXPECT_TRUE(is_safe_zip_entry_path("acecode.exe"));
    EXPECT_TRUE(is_safe_zip_entry_path("share/acecode/file.txt"));
    EXPECT_FALSE(is_safe_zip_entry_path("/abs/acecode.exe"));
    EXPECT_FALSE(is_safe_zip_entry_path("C:/abs/acecode.exe"));
    EXPECT_FALSE(is_safe_zip_entry_path("../acecode.exe"));
    EXPECT_FALSE(is_safe_zip_entry_path("dir/../acecode.exe"));
    EXPECT_FALSE(is_safe_zip_entry_path("dir\\acecode.exe"));
}

TEST(UpgradePackage, ValidatesRootOrSingleTopLevelPackage) {
    fs::path root = temp_root("acecode-staged-root");
    write_file(root / "acecode.exe", "exe");
    auto staged = validate_staged_package(root, "windows-x64", nullptr);
    ASSERT_TRUE(staged.has_value());
    EXPECT_EQ(staged->content_root, root);

    fs::path nested = temp_root("acecode-staged-nested");
    write_file(nested / "acecode-0.1.3" / "acecode.exe", "exe");
    auto nested_staged = validate_staged_package(nested, "windows-x64", nullptr);
    ASSERT_TRUE(nested_staged.has_value());
    EXPECT_EQ(nested_staged->content_root.filename(), "acecode-0.1.3");

    std::error_code ec;
    fs::remove_all(root, ec);
    fs::remove_all(nested, ec);
}
