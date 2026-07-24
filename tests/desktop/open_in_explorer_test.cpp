#include <gtest/gtest.h>

#include "desktop/open_in_explorer.hpp"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

using acecode::desktop::open_directory_in_file_manager;
using acecode::desktop::open_path_in_file_manager;
using acecode::desktop::append_allowed_open_root;
using acecode::desktop::OpenInExplorerTargetKind;
using acecode::desktop::validate_open_directory_request;
using acecode::desktop::validate_open_in_explorer_request;

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

TEST(DesktopOpenInExplorer, AllowsFileInsideRegisteredWorkspaceRoot) {
    auto root = make_tmp_dir("acecode_reveal_file_inside");
    auto file = root / "file.txt";
    std::ofstream(file.string()) << "x";

    auto result = validate_open_in_explorer_request(
        path_string(file),
        {path_string(root)});
    EXPECT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.kind, OpenInExplorerTargetKind::File);
    EXPECT_EQ(result.path.filename(), file.filename());

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

TEST(DesktopOpenInExplorer, RejectsFileOutsideRegisteredWorkspaceRoots) {
    auto root = make_tmp_dir("acecode_reveal_file_root");
    auto outside = make_tmp_dir("acecode_reveal_file_outside");
    auto file = outside / "file.txt";
    std::ofstream(file.string()) << "x";

    auto result = validate_open_in_explorer_request(
        path_string(file),
        {path_string(root)});
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

TEST(DesktopOpenInExplorer, AppendsGlobalSkillsRootOnce) {
    auto root = make_tmp_dir("acecode_open_explorer_root_append");
    auto skills = make_tmp_dir("acecode_open_explorer_skills_append");

    auto roots = append_allowed_open_root({path_string(root)}, path_string(skills));
    ASSERT_EQ(roots.size(), 2u);

    roots = append_allowed_open_root(std::move(roots), path_string(skills));
    EXPECT_EQ(roots.size(), 2u);

    auto result = validate_open_directory_request(path_string(skills), roots);
    EXPECT_TRUE(result.ok) << result.error;

    std::error_code ec;
    fs::remove_all(root, ec);
    fs::remove_all(skills, ec);
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

TEST(DesktopOpenInExplorer, FileRevealPassesFileKindToInjectedLauncher) {
    auto root = make_tmp_dir("acecode_reveal_file_launch");
    auto file = root / "file.txt";
    std::ofstream(file.string()) << "x";
    fs::path launched;
    auto launched_kind = OpenInExplorerTargetKind::Directory;

    auto result = open_path_in_file_manager(
        path_string(file),
        {path_string(root)},
        [&](const fs::path& path,
            OpenInExplorerTargetKind kind,
            std::string&) {
            launched = path;
            launched_kind = kind;
            return true;
        });

    EXPECT_TRUE(result.ok) << result.error;
    EXPECT_EQ(launched.filename(), file.filename());
    EXPECT_EQ(launched_kind, OpenInExplorerTargetKind::File);

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(DesktopOpenInExplorer, PathRevealPreservesDirectoryKind) {
    auto root = make_tmp_dir("acecode_reveal_directory_launch");
    auto launched_kind = OpenInExplorerTargetKind::File;

    auto result = open_path_in_file_manager(
        path_string(root),
        {path_string(root)},
        [&](const fs::path&,
            OpenInExplorerTargetKind kind,
            std::string&) {
            launched_kind = kind;
            return true;
        });

    EXPECT_TRUE(result.ok) << result.error;
    EXPECT_EQ(launched_kind, OpenInExplorerTargetKind::Directory);

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
