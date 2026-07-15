#include <gtest/gtest.h>

#include "desktop/desktop_restart.hpp"

#include <filesystem>
#include <fstream>
#include <random>

namespace fs = std::filesystem;
using acecode::desktop::validate_desktop_restart_target;

namespace {

class TempDir {
public:
    TempDir() {
        std::random_device rd;
        std::mt19937 rng(rd());
        std::uniform_int_distribution<int> dist(0, 0x7FFFFFFF);
        for (int attempt = 0; attempt < 8; ++attempt) {
            fs::path candidate = fs::temp_directory_path() /
                ("acecode_desktop_restart_" + std::to_string(dist(rng)));
            std::error_code ec;
            if (fs::create_directories(candidate, ec) && !ec) {
                path_ = candidate;
                return;
            }
        }
        ADD_FAILURE() << "could not create unique temp dir";
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

} // namespace

TEST(DesktopRestart, RejectsEmptyAndMissingTargets) {
    auto empty = validate_desktop_restart_target({});
    EXPECT_FALSE(empty.ok);
    EXPECT_NE(empty.error.find("empty"), std::string::npos);

    TempDir temp;
    auto missing = validate_desktop_restart_target(temp.path() / "missing.exe");
    EXPECT_FALSE(missing.ok);
    EXPECT_NE(missing.error.find("does not exist"), std::string::npos);
}

TEST(DesktopRestart, RejectsDirectoryTarget) {
    TempDir temp;
    auto result = validate_desktop_restart_target(temp.path());
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("regular file"), std::string::npos);
}

TEST(DesktopRestart, AcceptsExecutableFileTarget) {
    TempDir temp;
#ifdef _WIN32
    fs::path executable = temp.path() / "acecode-desktop.exe";
#else
    fs::path executable = temp.path() / "acecode-desktop";
#endif
    std::ofstream(executable, std::ios::binary) << "stub";
#ifndef _WIN32
    fs::permissions(executable,
                    fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
                    fs::perm_options::replace);
#endif

    auto result = validate_desktop_restart_target(executable);
    EXPECT_TRUE(result.ok) << result.error;
    EXPECT_TRUE(result.error.empty());
}
