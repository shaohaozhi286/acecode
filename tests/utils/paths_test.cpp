// 覆盖 src/utils/paths.hpp 的 RunMode 抽象 + 跨平台数据目录解析(spec design.md
// Decision 8)。验证:
//   1. 默认 RunMode 是 User
//   2. set_run_mode 是 once-only — 二次调用静默忽略,RunMode 不变
//   3. override_run_mode_for_test 给测试一个绕过 once-only 的合法路径
//   4. resolve_data_dir(Service) 按平台返回 deterministic 路径(Win=PROGRAMDATA / mac=/Library/... / linux=/var/lib/acecode)
//   5. resolve_data_dir(User) 始终以 ".acecode" 结尾(沿用历史 ~/.acecode 形态)
//   6. config::get_acecode_dir() 与当前 RunMode 同步切换
//
// 单例 RunMode 跨 TEST 会污染 → fixture TearDown 强制 reset_run_mode_for_test()。

#include <gtest/gtest.h>

#include "config/config.hpp"
#include "utils/paths.hpp"

#include <string>

namespace {

class PathsTest : public ::testing::Test {
protected:
    void SetUp() override {
        // SetUp 与 TearDown 都重置 — 防止前一条测试漏 TearDown 的脏状态泄漏过来
        acecode::reset_run_mode_for_test();
    }
    void TearDown() override {
        acecode::reset_run_mode_for_test();
    }
};

} // namespace

// 进程刚启动时 RunMode 必须是 User,否则 TUI 路径解析会被静默改到系统目录
TEST_F(PathsTest, DefaultRunModeIsUser) {
    EXPECT_EQ(acecode::get_run_mode(), acecode::RunMode::User);
}

// set_run_mode 必须是 once-only — 这是防呆,防止进程半路改根目录把状态散到两套
TEST_F(PathsTest, SetRunModeIsOnceOnly) {
    acecode::set_run_mode(acecode::RunMode::Service);
    EXPECT_EQ(acecode::get_run_mode(), acecode::RunMode::Service);

    // 第二次调用必须被吞掉,RunMode 保持 Service
    acecode::set_run_mode(acecode::RunMode::User);
    EXPECT_EQ(acecode::get_run_mode(), acecode::RunMode::Service)
        << "set_run_mode 应该是 once-only,二次调用必须被忽略";
}

// 测试 fixture 通过 override 绕过 once 保护;返回原值方便手工还原
TEST_F(PathsTest, OverrideRunModeForTestBypassesOnceGuard) {
    acecode::set_run_mode(acecode::RunMode::Service);

    auto prev = acecode::override_run_mode_for_test(acecode::RunMode::User);
    EXPECT_EQ(prev, acecode::RunMode::Service);
    EXPECT_EQ(acecode::get_run_mode(), acecode::RunMode::User);

    // 再 override 回 Service 也应工作 — 没有 once 限制
    auto prev2 = acecode::override_run_mode_for_test(acecode::RunMode::Service);
    EXPECT_EQ(prev2, acecode::RunMode::User);
    EXPECT_EQ(acecode::get_run_mode(), acecode::RunMode::Service);
}

// Service 模式三平台返回固定 / 半固定路径。Windows 走 PROGRAMDATA 环境变量,
// macOS / Linux 完全字面量,无环境依赖
TEST_F(PathsTest, ResolveDataDirServiceModeReturnsPlatformPath) {
    auto p = acecode::resolve_data_dir(acecode::RunMode::Service);
#if defined(_WIN32)
    // 典型值 C:\ProgramData\acecode;PROGRAMDATA 缺失退到 C:\ProgramData,
    // 子目录都是 acecode
    EXPECT_NE(p.find("acecode"), std::string::npos)
        << "Service 路径应包含 acecode 子目录: " << p;
    EXPECT_NE(p.find("ProgramData"), std::string::npos)
        << "Service 路径应在 ProgramData 下: " << p;
#elif defined(__APPLE__)
    EXPECT_EQ(p, "/Library/Application Support/acecode");
#else
    EXPECT_EQ(p, "/var/lib/acecode");
#endif
}

// User 模式三平台都以 ".acecode" 结尾(隐藏目录命名约定),根来自 HOME / USERPROFILE
TEST_F(PathsTest, ResolveDataDirUserModeEndsWithAcecode) {
    auto p = acecode::resolve_data_dir(acecode::RunMode::User);
    const std::string suffix = ".acecode";
    ASSERT_GE(p.size(), suffix.size());
    EXPECT_EQ(p.substr(p.size() - suffix.size()), suffix)
        << "User 模式数据目录应以 .acecode 结尾(三平台一致): " << p;
}

// 验证 config::get_acecode_dir 与 RunMode 同步:override 切到 Service 后,路径
// 必须切到系统级目录;切回 User 必须复原。这是 daemon Service 模式正确性的核心
TEST_F(PathsTest, GetAcecodeDirSyncsWithRunMode) {
    // 起步 User
    auto user_path = acecode::get_acecode_dir();
    EXPECT_EQ(user_path, acecode::resolve_data_dir(acecode::RunMode::User));

    // 切到 Service
    acecode::override_run_mode_for_test(acecode::RunMode::Service);
    auto svc_path = acecode::get_acecode_dir();
    EXPECT_EQ(svc_path, acecode::resolve_data_dir(acecode::RunMode::Service));
    EXPECT_NE(svc_path, user_path)
        << "Service 路径必须与 User 路径不同(否则 daemon/TUI 会撞数据)";

    // 切回 User
    acecode::override_run_mode_for_test(acecode::RunMode::User);
    EXPECT_EQ(acecode::get_acecode_dir(), user_path);
}

// reset 应该把 RunMode 与 once-only 标记一起清回去 — 这是 fixture 间隔离的基石
TEST_F(PathsTest, ResetClearsBothModeAndOnceFlag) {
    acecode::set_run_mode(acecode::RunMode::Service);
    acecode::reset_run_mode_for_test();
    EXPECT_EQ(acecode::get_run_mode(), acecode::RunMode::User);

    // reset 后 set_run_mode 应能再次生效(once 标记已清)
    acecode::set_run_mode(acecode::RunMode::Service);
    EXPECT_EQ(acecode::get_run_mode(), acecode::RunMode::Service);
}

// ─── get_project_dirs_up_to_home:映射盘视图下的 HOME 判定 ─────────────────
//
// 背景 bug:用户把工作区放在 subst/映射盘(如 N: → C:\ 的视图)时,daemon 的
// cwd 是 N:\Users\me\repo 而 USERPROFILE 是 C:\Users\me。旧实现只做字面路径
// 比较,项目链越过 HOME 一路冲到 N:\ 盘根 —— HOME 级目录(~/.claude 等)被
// 卷成"项目链",Web 设置页里全局技能清零、全部错标成项目技能(实测复现)。
// 修复 = 比较时补一次 fs::equivalent 物理等价判定。
//
// Windows 上用 directory junction(mklink /J,无需管理员)模拟映射盘视图;
// POSIX 上用符号链接。创建失败(权限/文件系统不支持)则 SKIP。

#include "utils/utf8_path.hpp"

#include <cstdlib>
#include <filesystem>
#include <random>

namespace {

namespace fs = std::filesystem;

// 场景: HOME=<root>/real-home,cwd=<root>/link-home/work/repo,其中 link-home
// 是指向 real-home 的 junction/symlink(字面不同、物理相同)。期望项目链在
// link-home 处停下:包含 repo 与 work,不包含 link-home 本身及更上层。
TEST_F(PathsTest, ProjectDirsStopAtHomeThroughDirectoryJunction) {
    std::random_device rd;
    fs::path root = fs::temp_directory_path() /
                    ("acecode_paths_junction_" + std::to_string(rd()));
    fs::path real_home = root / "real-home";
    fs::path link_home = root / "link-home";
    std::error_code ec;
    fs::create_directories(real_home / "work" / "repo", ec);
    ASSERT_FALSE(ec);

#ifdef _WIN32
    // mklink /J 创建 directory junction,普通用户权限即可。
    std::string cmd = "mklink /J \"" + acecode::path_to_utf8(link_home) + "\" \"" +
                      acecode::path_to_utf8(real_home) + "\" >NUL 2>&1";
    if (std::system(("cmd /c " + cmd).c_str()) != 0) {
        fs::remove_all(root, ec);
        GTEST_SKIP() << "cannot create directory junction here";
    }
#else
    fs::create_directory_symlink(real_home, link_home, ec);
    if (ec) {
        fs::remove_all(root, ec);
        GTEST_SKIP() << "cannot create directory symlink here";
    }
#endif

#ifdef _WIN32
    const char* home_var = "USERPROFILE";
#else
    const char* home_var = "HOME";
#endif
    std::string saved_home;
    if (const char* prev = std::getenv(home_var)) saved_home = prev;
#ifdef _WIN32
    _putenv_s(home_var, acecode::path_to_utf8(real_home).c_str());
#else
    setenv(home_var, acecode::path_to_utf8(real_home).c_str(), 1);
#endif

    auto dirs = acecode::get_project_dirs_up_to_home(
        acecode::path_to_utf8(link_home / "work" / "repo"));

    // 恢复环境变量再断言,避免失败时污染后续测试
#ifdef _WIN32
    _putenv_s(home_var, saved_home.c_str());
#else
    if (saved_home.empty()) unsetenv(home_var); else setenv(home_var, saved_home.c_str(), 1);
#endif

    ASSERT_EQ(dirs.size(), 2u)
        << "项目链应止于 HOME 的 junction 视图(repo + work),实际: "
        << [&] { std::string s; for (auto& d : dirs) { s += d; s += " | "; } return s; }();
    EXPECT_NE(dirs[0].find("repo"), std::string::npos);
    EXPECT_NE(dirs[1].find("work"), std::string::npos);

    fs::remove_all(root, ec);
}

} // namespace
