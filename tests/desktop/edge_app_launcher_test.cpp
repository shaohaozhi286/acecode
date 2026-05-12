#include <gtest/gtest.h>

#include "desktop/edge_app_launcher.hpp"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <system_error>

namespace fs = std::filesystem;
using acecode::desktop::build_edge_app_parameters_w;
using acecode::desktop::find_msedge_executable_in;

namespace {

class TempDir {
public:
    TempDir() {
        std::random_device rd;
        std::mt19937 rng(rd());
        std::uniform_int_distribution<int> dist(0, 0x7FFFFFFF);
        for (int attempt = 0; attempt < 8; ++attempt) {
            fs::path candidate = fs::temp_directory_path() /
                ("acecode_edge_app_launcher_" + std::to_string(dist(rng)));
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

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

fs::path make_msedge_exe(const fs::path& root) {
    fs::path exe = root / "Microsoft" / "Edge" / "Application" / "msedge.exe";
    std::error_code ec;
    fs::create_directories(exe.parent_path(), ec);
    EXPECT_FALSE(ec) << ec.message();
    std::ofstream(exe) << "stub";
    return exe;
}

} // namespace

TEST(EdgeAppLauncher, EmptyRootsReturnsNullopt) {
    EXPECT_FALSE(find_msedge_executable_in({}).has_value());
}

TEST(EdgeAppLauncher, FindsMsedgeUnderEdgeApplicationRoot) {
    TempDir d;
    fs::path expected = make_msedge_exe(d.path());

    auto result = find_msedge_executable_in({d.path()});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->lexically_normal(), expected.lexically_normal());
}

TEST(EdgeAppLauncher, SkipsRootsWithoutMsedge) {
    TempDir empty;
    TempDir with_edge;
    fs::path expected = make_msedge_exe(with_edge.path());

    auto result = find_msedge_executable_in({empty.path(), with_edge.path()});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->lexically_normal(), expected.lexically_normal());
}

TEST(EdgeAppLauncher, BuildsAppModeParametersAndQuotesProfilePath) {
    std::wstring params = build_edge_app_parameters_w(
        L"http://127.0.0.1:28080/?token=a%20b",
        L"C:\\Users\\Test User\\.acecode\\edge-app-profile");

    EXPECT_NE(params.find(L"--app=http://127.0.0.1:28080/?token=a%20b"), std::wstring::npos);
    EXPECT_NE(params.find(L"\"--user-data-dir=C:\\Users\\Test User\\.acecode\\edge-app-profile\""),
              std::wstring::npos);
    EXPECT_NE(params.find(L"--no-first-run"), std::wstring::npos);
}
