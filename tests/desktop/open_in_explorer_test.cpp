#include <gtest/gtest.h>

#include "desktop/open_in_explorer.hpp"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

using acecode::desktop::open_directory_in_file_manager;
using acecode::desktop::validate_open_directory_request;

namespace {

fs::path make_tmp_dir(const std::string& name) {
    auto base = fs::temp_directory_path() / (name + "_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base, ec);
    return base;
}

std::string path_string(const fs::path& path) {
    return path.string();
}

} // namespace

TEST(DesktopOpenInExplorer, RejectsEmptyAndRelativePaths) {
    EXPECT_FALSE(validate_open_directory_request("").ok);
    auto rel = validate_open_directory_request("relative/path");
    EXPECT_FALSE(rel.ok);
    EXPECT_NE(rel.error.find("absolute"), std::string::npos);
}

TEST(DesktopOpenInExplorer, RejectsMissingDirectoryAndFiles) {
    auto root = make_tmp_dir("acecode_open_explorer_missing");
    auto missing = validate_open_directory_request(path_string(root / "missing"), {path_string(root)});
    EXPECT_FALSE(missing.ok);

    auto file = root / "file.txt";
    std::ofstream(file.string()) << "x";
    auto not_dir = validate_open_directory_request(path_string(file), {path_string(root)});
    EXPECT_FALSE(not_dir.ok);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(DesktopOpenInExplorer, AllowsDirectoryInsideRegisteredWorkspaceRoot) {
    auto root = make_tmp_dir("acecode_open_explorer_inside");
    auto nested = root / "a" / "b";
    fs::create_directories(nested);

    auto result = validate_open_directory_request(path_string(nested), {path_string(root)});
    EXPECT_TRUE(result.ok) << result.error;

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(DesktopOpenInExplorer, RejectsDirectoryOutsideRegisteredWorkspaceRoots) {
    auto root = make_tmp_dir("acecode_open_explorer_root");
    auto outside = make_tmp_dir("acecode_open_explorer_outside");

    auto result = validate_open_directory_request(path_string(outside), {path_string(root)});
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("outside"), std::string::npos);

    std::error_code ec;
    fs::remove_all(root, ec);
    fs::remove_all(outside, ec);
}

TEST(DesktopOpenInExplorer, AllowsExistingDirectoryWhenNoRegistryRootsProvided) {
    auto root = make_tmp_dir("acecode_open_explorer_no_roots");

    auto result = validate_open_directory_request(path_string(root));
    EXPECT_TRUE(result.ok) << result.error;

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(DesktopOpenInExplorer, UsesInjectedLauncherAfterValidation) {
    auto root = make_tmp_dir("acecode_open_explorer_launch");
    fs::path launched;

    auto result = open_directory_in_file_manager(path_string(root), {path_string(root)},
        [&](const fs::path& path, std::string&) {
            launched = path;
            return true;
        });

    EXPECT_TRUE(result.ok) << result.error;
    EXPECT_FALSE(launched.empty());

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(DesktopOpenInExplorer, PropagatesInjectedLauncherFailure) {
    auto root = make_tmp_dir("acecode_open_explorer_launch_fail");

    auto result = open_directory_in_file_manager(path_string(root), {path_string(root)},
        [](const fs::path&, std::string& error) {
            error = "mock failure";
            return false;
        });

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, "mock failure");

    std::error_code ec;
    fs::remove_all(root, ec);
}
