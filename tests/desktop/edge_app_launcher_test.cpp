#include <gtest/gtest.h>

#include "desktop/edge_app_launcher.hpp"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <system_error>

namespace fs = std::filesystem;
using acecode::desktop::build_edge_app_parameters_w;
using acecode::desktop::edge_profile_subdir_name;
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

// 场景:两个不同进程(不同 PID)同时/先后回退到 Edge。
// 期望:profile 子目录名互不相同。
// 为什么重要(回归):固定共享 profile 时,残留的旧 msedge 会触发 Chromium 的
// single-instance 转交 —— 新启动的进程把 URL 交给旧实例后立刻退出,desktop 误以为
// 窗口已关 → 提前 stop_all 杀掉 daemon → Edge 窗口白屏。按 PID 唯一化子目录正是
// 为了让本次启动永远拿到一个没有旧实例占用的干净 profile。
TEST(EdgeAppLauncher, ProfileSubdirNameIsUniquePerPid) {
    EXPECT_NE(edge_profile_subdir_name(1234), edge_profile_subdir_name(5678));
}

// 场景:生成 profile 子目录名。
// 期望:固定 "u" 前缀 + 十进制 PID。前缀避免纯数字目录名在某些工具/路径解析里被
// 误当作别的东西,也便于一眼识别是 acecode 造的 per-launch profile。
TEST(EdgeAppLauncher, ProfileSubdirNameHasPrefixAndDecimalPid) {
    EXPECT_EQ(edge_profile_subdir_name(42), "u42");
    EXPECT_EQ(edge_profile_subdir_name(0), "u0");
}

// 场景:同一进程内多次取名(理论上只会调一次,但防御性确认无随机/计数副作用)。
// 期望:幂等,完全相同。
TEST(EdgeAppLauncher, ProfileSubdirNameIsStableForSamePid) {
    EXPECT_EQ(edge_profile_subdir_name(99999), edge_profile_subdir_name(99999));
}
