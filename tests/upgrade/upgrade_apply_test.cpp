#include "upgrade/apply.hpp"
#include "upgrade/package.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

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

std::string read_file(const fs::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

} // namespace

TEST(UpgradeApply, BuildsAndParsesRunnerArguments) {
    ApplyOptions opts;
    opts.parent_pid = 1234;
    opts.staging_dir = "C:/Temp/staging dir";
    opts.install_dir = "C:/Program Files/ACECode";
    opts.backup_dir = "C:/Temp/backup";

    auto args = build_apply_runner_args(opts);
    ASSERT_EQ(args.front(), "--apply-update");

    std::string err;
    std::vector<std::string> tail(args.begin() + 1, args.end());
    auto parsed = parse_apply_runner_args(tail, &err);
    ASSERT_TRUE(parsed.has_value()) << err;
    EXPECT_EQ(parsed->parent_pid, opts.parent_pid);
    EXPECT_EQ(parsed->staging_dir, opts.staging_dir);
    EXPECT_EQ(parsed->install_dir, opts.install_dir);
    EXPECT_EQ(parsed->backup_dir, opts.backup_dir);
    EXPECT_EQ(quote_command_arg("C:/Program Files/ACECode/acecode.exe"),
              "\"C:/Program Files/ACECode/acecode.exe\"");
}

TEST(UpgradeApply, AppliesStagedUpdateAndKeepsBackup) {
    fs::path root = temp_root("acecode-apply");
    fs::path install = root / "install";
    fs::path staging = root / "staging";
    fs::path backup = root / "backup";

    write_file(install / "old.txt", "old");
    write_file(staging / "acecode.exe", "new exe");
    write_file(staging / "share" / "asset.txt", "asset");

    std::string err;
    ASSERT_TRUE(apply_staged_update(staging, install, backup, "windows-x64", &err)) << err;
    EXPECT_EQ(read_file(install / "acecode.exe"), "new exe");
    EXPECT_EQ(read_file(install / "share" / "asset.txt"), "asset");
    EXPECT_EQ(read_file(backup / "old.txt"), "old");

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(UpgradeApply, InvalidStageDoesNotModifyInstall) {
    fs::path root = temp_root("acecode-apply-invalid");
    fs::path install = root / "install";
    fs::path staging = root / "staging";
    fs::path backup = root / "backup";

    write_file(install / "old.txt", "old");
    write_file(staging / "readme.txt", "missing exe");

    std::string err;
    EXPECT_FALSE(apply_staged_update(staging, install, backup, "windows-x64", &err));
    EXPECT_EQ(read_file(install / "old.txt"), "old");

    std::error_code ec;
    fs::remove_all(root, ec);
}
