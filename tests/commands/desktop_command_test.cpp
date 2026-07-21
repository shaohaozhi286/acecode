#include <gtest/gtest.h>

#include "commands/desktop_command.hpp"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

class TempDir {
public:
    TempDir() {
        std::random_device random;
        for (int attempt = 0; attempt < 8; ++attempt) {
            const auto suffix = std::to_string(random());
            fs::path candidate = fs::temp_directory_path() /
                                 ("acecode_desktop_command_" + suffix);
            std::error_code ec;
            if (fs::create_directories(candidate, ec) && !ec) {
                path_ = std::move(candidate);
                return;
            }
        }
        ADD_FAILURE() << "could not create a unique temporary directory";
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

const char* desktop_executable_name() {
#ifdef _WIN32
    return "acecode-desktop.exe";
#else
    return "acecode-desktop";
#endif
}

fs::path create_desktop_executable(const fs::path& directory) {
    fs::path executable = directory / desktop_executable_name();
    std::ofstream(executable, std::ios::binary) << "stub";
#ifndef _WIN32
    fs::permissions(executable,
                    fs::perms::owner_read | fs::perms::owner_write |
                        fs::perms::owner_exec,
                    fs::perm_options::replace);
#endif
    return executable;
}

} // namespace

TEST(DesktopCommand, ResolvesDesktopBesideAcecodeExecutable) {
    TempDir temp;
#ifdef _WIN32
    const fs::path acecode_executable = temp.path() / "bin" / "acecode.exe";
#else
    const fs::path acecode_executable = temp.path() / "bin" / "acecode";
#endif

    EXPECT_EQ(acecode::sibling_desktop_executable(acecode_executable),
              acecode_executable.parent_path() / desktop_executable_name());
    EXPECT_TRUE(acecode::sibling_desktop_executable({}).empty());
}

TEST(DesktopCommand, RejectsEmptyAndMissingDesktopPathsWithoutSpawning) {
    int spawn_calls = 0;
    auto spawner = [&](const std::vector<std::string>&) {
        ++spawn_calls;
        return true;
    };

    auto empty = acecode::launch_sibling_desktop({}, spawner);
    EXPECT_FALSE(empty.ok);
    EXPECT_NE(empty.error.find("cannot resolve"), std::string::npos);

    TempDir temp;
#ifdef _WIN32
    const fs::path acecode_executable = temp.path() / "acecode.exe";
#else
    const fs::path acecode_executable = temp.path() / "acecode";
#endif
    auto missing = acecode::launch_sibling_desktop(acecode_executable, spawner);
    EXPECT_FALSE(missing.ok);
    EXPECT_NE(missing.error.find("does not exist"), std::string::npos);
    EXPECT_EQ(spawn_calls, 0);
}

TEST(DesktopCommand, RejectsDirectoryAtDesktopExecutablePath) {
    TempDir temp;
#ifdef _WIN32
    const fs::path acecode_executable = temp.path() / "acecode.exe";
#else
    const fs::path acecode_executable = temp.path() / "acecode";
#endif
    ASSERT_TRUE(fs::create_directory(temp.path() / desktop_executable_name()));

    bool spawned = false;
    auto result = acecode::launch_sibling_desktop(
        acecode_executable,
        [&](const std::vector<std::string>&) {
            spawned = true;
            return true;
        });

    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("regular file"), std::string::npos);
    EXPECT_FALSE(spawned);
}

TEST(DesktopCommand, SpawnsOnlyTheExactSiblingExecutable) {
    TempDir temp;
#ifdef _WIN32
    const fs::path acecode_executable = temp.path() / "acecode.exe";
#else
    const fs::path acecode_executable = temp.path() / "acecode";
#endif
    const fs::path desktop_executable = create_desktop_executable(temp.path());
    std::vector<std::string> spawned_argv;

    auto result = acecode::launch_sibling_desktop(
        acecode_executable,
        [&](const std::vector<std::string>& argv) {
            spawned_argv = argv;
            return true;
        });

    EXPECT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.executable, desktop_executable);
    ASSERT_EQ(spawned_argv.size(), 1u);
    EXPECT_EQ(fs::path(spawned_argv.front()), desktop_executable);
}

TEST(DesktopCommand, ReportsDetachedSpawnFailure) {
    TempDir temp;
#ifdef _WIN32
    const fs::path acecode_executable = temp.path() / "acecode.exe";
#else
    const fs::path acecode_executable = temp.path() / "acecode";
#endif
    const fs::path desktop_executable = create_desktop_executable(temp.path());

    auto result = acecode::launch_sibling_desktop(
        acecode_executable,
        [](const std::vector<std::string>&) { return false; });

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.executable, desktop_executable);
    EXPECT_NE(result.error.find("failed to create detached desktop process"),
              std::string::npos);
}

#ifndef _WIN32
TEST(DesktopCommand, RejectsNonExecutablePosixDesktopFile) {
    TempDir temp;
    const fs::path acecode_executable = temp.path() / "acecode";
    const fs::path desktop_executable = temp.path() / desktop_executable_name();
    std::ofstream(desktop_executable, std::ios::binary) << "stub";
    fs::permissions(desktop_executable,
                    fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace);

    bool spawned = false;
    auto result = acecode::launch_sibling_desktop(
        acecode_executable,
        [&](const std::vector<std::string>&) {
            spawned = true;
            return true;
        });

    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("not executable"), std::string::npos);
    EXPECT_FALSE(spawned);
}
#endif
