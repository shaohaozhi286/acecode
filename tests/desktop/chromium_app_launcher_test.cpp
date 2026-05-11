// 覆盖 src/desktop/chromium_app_launcher.cpp 中的纯函数 find_chromium_app_browser_in。
// 系统调用版 find_chromium_app_browser() 依赖 SHGetKnownFolderPath,unit test
// 直接喂临时目录列表更直观。launch_chromium_app_mode() 启 CreateProcessW
// 无法在 headless CI 跑,留作手动 e2e。

#include <gtest/gtest.h>

#include "desktop/chromium_app_launcher.hpp"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using acecode::desktop::find_chromium_app_browser_in;

namespace {

class TempDir {
public:
    TempDir() {
        std::random_device rd;
        std::mt19937 rng(rd());
        std::uniform_int_distribution<int> dist(0, 0x7FFFFFFF);
        for (int attempt = 0; attempt < 8; ++attempt) {
            const std::string name = "acecode_chromium_app_" +
                                     std::to_string(dist(rng));
            fs::path candidate = fs::temp_directory_path() / name;
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

fs::path make_browser_exe(const fs::path& root, const std::string& rel) {
    fs::path full = root / rel;
    std::error_code ec;
    fs::create_directories(full.parent_path(), ec);
    EXPECT_FALSE(ec) << ec.message();
    std::ofstream(full) << "stub";
    return full;
}

} // namespace

TEST(ChromiumAppLauncher, EmptyRootsReturnsNullopt) {
    EXPECT_FALSE(find_chromium_app_browser_in({}).has_value());
}

TEST(ChromiumAppLauncher, RootWithoutAnyBrowserReturnsNullopt) {
    TempDir d;
    EXPECT_FALSE(find_chromium_app_browser_in({d.path()}).has_value());
}

// 单 root 装了 Edge → 命中 Edge,display_name 标记正确。
TEST(ChromiumAppLauncher, FindsEdgeInProgramFiles) {
    TempDir d;
    fs::path edge = make_browser_exe(d.path(), "Microsoft/Edge/Application/msedge.exe");
    auto r = find_chromium_app_browser_in({d.path()});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->exe.lexically_normal(), edge.lexically_normal());
    EXPECT_EQ(r->display_name, "Microsoft Edge");
}

// 单 root 装了 Chrome → 命中 Chrome。
TEST(ChromiumAppLauncher, FindsChromeInProgramFiles) {
    TempDir d;
    fs::path chrome = make_browser_exe(d.path(), "Google/Chrome/Application/chrome.exe");
    auto r = find_chromium_app_browser_in({d.path()});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->exe.lexically_normal(), chrome.lexically_normal());
    EXPECT_EQ(r->display_name, "Google Chrome");
}

// 同一 root 同时装 Edge + Chrome → Edge 优先(用户体感更接近 native app)。
TEST(ChromiumAppLauncher, EdgePreferredOverChromeWhenBothPresent) {
    TempDir d;
    fs::path edge = make_browser_exe(d.path(), "Microsoft/Edge/Application/msedge.exe");
    make_browser_exe(d.path(), "Google/Chrome/Application/chrome.exe");
    auto r = find_chromium_app_browser_in({d.path()});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->exe.lexically_normal(), edge.lexically_normal());
    EXPECT_EQ(r->display_name, "Microsoft Edge");
}

// 两个 root,Edge 在第二个,Chrome 在第一个 → 仍然 Edge 优先(按候选浏览器
// 顺序而不是 root 顺序,避免 PFx86 只装 Chrome 时漏掉 PF 的 Edge)。
TEST(ChromiumAppLauncher, EdgePreferredAcrossRoots) {
    TempDir pf, pfx86;
    make_browser_exe(pfx86.path(), "Google/Chrome/Application/chrome.exe");
    fs::path edge = make_browser_exe(pf.path(), "Microsoft/Edge/Application/msedge.exe");
    auto r = find_chromium_app_browser_in({pf.path(), pfx86.path()});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->exe.lexically_normal(), edge.lexically_normal());
}

// 空字符串 root 元素静默跳过(防御性 — SHGetKnownFolderPath 失败可能塞空)。
TEST(ChromiumAppLauncher, EmptyRootEntryIgnored) {
    TempDir d;
    fs::path edge = make_browser_exe(d.path(), "Microsoft/Edge/Application/msedge.exe");
    auto r = find_chromium_app_browser_in({fs::path(), d.path()});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->exe.lexically_normal(), edge.lexically_normal());
}

// 路径上是目录而非文件 → 不算命中。
TEST(ChromiumAppLauncher, DirectoryAtExePathIsNotAMatch) {
    TempDir d;
    std::error_code ec;
    fs::create_directories(d.path() / "Microsoft/Edge/Application/msedge.exe", ec);
    EXPECT_FALSE(find_chromium_app_browser_in({d.path()}).has_value());
}
