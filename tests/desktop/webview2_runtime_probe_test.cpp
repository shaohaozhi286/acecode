// 覆盖 src/desktop/webview2_runtime_probe.cpp 中的纯函数 find_edge_browser_folder_in。
// 系统调用版 find_edge_browser_folder() 不在测试范围 — 它依赖 SHGetKnownFolderPath,
// 在 unit test 进程里直接喂临时目录列表更直观。

#include <gtest/gtest.h>

#include "desktop/webview2_runtime_probe.hpp"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using acecode::desktop::find_edge_browser_folder_in;

namespace {

// 单测用临时根目录,析构时清理。各 case 独立目录,避免并发污染。
class TempDir {
public:
    TempDir() {
        std::random_device rd;
        std::mt19937 rng(rd());
        std::uniform_int_distribution<int> dist(0, 0x7FFFFFFF);
        for (int attempt = 0; attempt < 8; ++attempt) {
            const std::string name = "acecode_webview2_probe_" +
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

// 在 root 下造一个 Edge 浏览器风格的版本子目录,可选地写一个 placeholder
// msedgewebview2.exe(传 false 时只造目录、不造文件 — 用于覆盖
// "目录在但 exe 缺失" 的过滤分支)。
fs::path make_version_folder(const fs::path& root,
                             const std::string& version,
                             bool with_exe = true) {
    fs::path application = root / "Microsoft" / "Edge" / "Application";
    fs::path folder = application / version;
    std::error_code ec;
    fs::create_directories(folder, ec);
    EXPECT_FALSE(ec) << ec.message();
    if (with_exe) {
        std::ofstream(folder / "msedgewebview2.exe") << "stub";
    }
    return folder;
}

} // namespace

// 完全空的根目录列表 → nullopt
TEST(Webview2RuntimeProbe, EmptyRootsReturnsNullopt) {
    EXPECT_FALSE(find_edge_browser_folder_in({}).has_value());
}

// 根目录存在但里面没有 Edge\Application → nullopt
TEST(Webview2RuntimeProbe, RootWithoutEdgeReturnsNullopt) {
    TempDir d;
    auto result = find_edge_browser_folder_in({d.path()});
    EXPECT_FALSE(result.has_value());
}

// 单一版本子目录 + msedgewebview2.exe → 命中并返回该目录
TEST(Webview2RuntimeProbe, SingleVersionWithExeIsPicked) {
    TempDir d;
    fs::path expected = make_version_folder(d.path(), "120.0.2210.91");
    auto result = find_edge_browser_folder_in({d.path()});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->lexically_normal(), expected.lexically_normal());
}

// 多个合法版本 → 选 4 段版本号最大的
TEST(Webview2RuntimeProbe, PicksLatestVersionByNumericCompare) {
    TempDir d;
    make_version_folder(d.path(), "100.0.1185.39");
    make_version_folder(d.path(), "120.0.2210.91");
    fs::path latest = make_version_folder(d.path(), "131.0.2903.86");
    make_version_folder(d.path(), "129.0.2792.79");

    auto result = find_edge_browser_folder_in({d.path()});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->lexically_normal(), latest.lexically_normal());
}

// 字典序陷阱:"99.0.1.1" 字符串字典序 > "100.0.1.1",但数值 99 < 100,
// 实现必须按数值比较。
TEST(Webview2RuntimeProbe, NumericNotLexicographicVersionOrder) {
    TempDir d;
    make_version_folder(d.path(), "99.0.1.1");
    fs::path latest = make_version_folder(d.path(), "100.0.1.1");

    auto result = find_edge_browser_folder_in({d.path()});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->lexically_normal(), latest.lexically_normal());
}

// 非 4 段版本号格式的目录(SetupMetrics、Installer、3 段、空段、字母)被忽略
TEST(Webview2RuntimeProbe, FiltersNonVersionFolders) {
    TempDir d;
    make_version_folder(d.path(), "SetupMetrics");
    make_version_folder(d.path(), "Installer");
    make_version_folder(d.path(), "100.0.1234"); // 只 3 段
    make_version_folder(d.path(), "100..0.1.1"); // 含空段
    make_version_folder(d.path(), "100.0.1.1a"); // 含字母
    fs::path good = make_version_folder(d.path(), "120.0.2210.91");

    auto result = find_edge_browser_folder_in({d.path()});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->lexically_normal(), good.lexically_normal());
}

// 版本目录在但 msedgewebview2.exe 缺失(Edge channel 切换残留场景)→ 跳过。
// 否则会把死路径喂给 SetEnvironmentVariableW,WebView2 加载时再炸一次。
TEST(Webview2RuntimeProbe, SkipsVersionFolderWithoutExecutable) {
    TempDir d;
    make_version_folder(d.path(), "131.0.2903.86", /*with_exe=*/false);
    fs::path with_exe = make_version_folder(d.path(), "120.0.2210.91");

    auto result = find_edge_browser_folder_in({d.path()});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->lexically_normal(), with_exe.lexically_normal());
}

// 多个 root 都装了 Edge:返回所有 root 命中里版本号最大的(模拟 PF 与 PFx86
// 都装了 Edge 的机器,选最新一份)。
TEST(Webview2RuntimeProbe, AcrossMultipleRootsPicksGlobalLatest) {
    TempDir pf;
    TempDir pfx86;
    make_version_folder(pf.path(), "120.0.2210.91");
    fs::path latest = make_version_folder(pfx86.path(), "131.0.2903.86");

    auto result = find_edge_browser_folder_in({pf.path(), pfx86.path()});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->lexically_normal(), latest.lexically_normal());
}

// 空字符串 root 元素被静默跳过(防御性 — SHGetKnownFolderPath 失败时
// production 代码会插入空 path)。
TEST(Webview2RuntimeProbe, EmptyRootEntryIgnored) {
    TempDir d;
    fs::path good = make_version_folder(d.path(), "120.0.2210.91");
    auto result = find_edge_browser_folder_in({fs::path(), d.path()});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->lexically_normal(), good.lexically_normal());
}
