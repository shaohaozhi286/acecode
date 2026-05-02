// 覆盖 src/desktop/workspace_registry.cpp。WorkspaceRegistry 是 desktop 的多
// workspace 模型基石 — 默认命名错、损坏文件挂、原子写不原子,任何一项跑偏
// 用户都能立刻在 sidebar 上看到。所有写盘路径走 tmp dir 以免污染 ~/.acecode。

#include <gtest/gtest.h>

#include "desktop/workspace_registry.hpp"
#include "utils/cwd_hash.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

using acecode::compute_cwd_hash;
using acecode::desktop::WorkspaceMeta;
using acecode::desktop::WorkspaceRegistry;
using acecode::desktop::default_workspace_name;

namespace {

// 测试 fixture: 每个 TEST 独立 tmp dir,析构时清理。
class TmpProjectsDir {
public:
    TmpProjectsDir() {
        auto base = fs::temp_directory_path() / "acecode_workspace_registry_test";
        fs::remove_all(base);
        fs::create_directories(base);
        path_ = base.string();
    }
    ~TmpProjectsDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    const std::string& path() const { return path_; }
private:
    std::string path_;
};

// 直接写一个 workspace.json,用于覆盖 scan / 损坏 fallback 的 setup 路径。
void seed_workspace_json(const std::string& projects_dir,
                         const std::string& hash,
                         const std::string& cwd,
                         const std::string& name,
                         const std::string& raw_override = {}) {
    fs::create_directories(fs::path(projects_dir) / hash);
    std::ofstream ofs((fs::path(projects_dir) / hash / "workspace.json").string());
    if (!raw_override.empty()) {
        ofs << raw_override; // 用于损坏 JSON 测试
        return;
    }
    nlohmann::json j;
    j["cwd"] = cwd;
    j["name"] = name;
    ofs << j.dump();
}

} // namespace

// 场景: POSIX 路径默认命名 = basename
TEST(WorkspaceRegistryDefault, PosixBasename) {
    EXPECT_EQ(default_workspace_name("/home/shao/proj"), "proj");
}

// 场景: Windows 反斜杠路径 默认命名 = basename
TEST(WorkspaceRegistryDefault, WindowsBackslashBasename) {
    EXPECT_EQ(default_workspace_name("N:\\Users\\shao\\acecode"), "acecode");
}

// 场景: 尾斜杠不影响默认命名("foo/" 仍是 "foo")
TEST(WorkspaceRegistryDefault, TrailingSlashBasename) {
    EXPECT_EQ(default_workspace_name("/home/shao/proj/"), "proj");
    EXPECT_EQ(default_workspace_name("/home/shao/proj//"), "proj");
}

// 场景: 根路径走 root_name 兜底("C:\\" → "C:" 在 Windows;POSIX 则继续到字面常量)
TEST(WorkspaceRegistryDefault, RootPathFallback) {
    auto v = default_workspace_name("/");
    // 在 POSIX 上 "/" 的 basename 为空 + root_name 也为空 → 走"workspace"字面;
    // 在 Windows 上 "/" 解析为根路径,root_name 通常为空 → 同样走"workspace"。
    // 不论平台,结果只可能是 root_name (非空) 或 "workspace"。
    EXPECT_FALSE(v.empty());
}

// 场景: 完全空字符串 cwd → 字面 "workspace"(避免空字符串当 name 显示)
TEST(WorkspaceRegistryDefault, EmptyCwdLiteralFallback) {
    EXPECT_EQ(default_workspace_name(""), "workspace");
}

// 场景: 空目录 scan 后 list 为空(不该崩,不该误造条目)
TEST(WorkspaceRegistry, ScanEmptyDir) {
    TmpProjectsDir tmp;
    WorkspaceRegistry r;
    r.scan(tmp.path());
    EXPECT_TRUE(r.list().empty());
}

// 场景: 不存在的 projects_dir → list 为空,不抛异常
TEST(WorkspaceRegistry, ScanMissingDir) {
    WorkspaceRegistry r;
    r.scan("/tmp/this/path/does/not/exist/abcxyz");
    EXPECT_TRUE(r.list().empty());
}

// 场景: scan 入册带合法 workspace.json 的子目录
TEST(WorkspaceRegistry, ScanLoadsValidEntry) {
    TmpProjectsDir tmp;
    seed_workspace_json(tmp.path(), "abcdef0123456789", "/home/x/y", "myname");

    WorkspaceRegistry r;
    r.scan(tmp.path());
    auto v = r.list();
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0].hash, "abcdef0123456789");
    EXPECT_EQ(v[0].cwd, "/home/x/y");
    EXPECT_EQ(v[0].name, "myname");
}

// 场景: scan 跳过缺 workspace.json 的孤儿目录(老 SessionManager 写过 sessions 但没 metadata)
TEST(WorkspaceRegistry, ScanSkipsOrphanDir) {
    TmpProjectsDir tmp;
    fs::create_directories(fs::path(tmp.path()) / "deadbeef00000000");
    // 故意不写 workspace.json
    WorkspaceRegistry r;
    r.scan(tmp.path());
    EXPECT_TRUE(r.list().empty());
}

// 场景: 损坏 JSON workspace.json 走 fallback — 不入册,且不删除原文件
TEST(WorkspaceRegistry, ScanCorruptedJsonGracefullySkips) {
    TmpProjectsDir tmp;
    seed_workspace_json(tmp.path(), "abcdef0123456789", "", "", "{ this is not json");

    WorkspaceRegistry r;
    r.scan(tmp.path());
    EXPECT_TRUE(r.list().empty());

    // 原文件应仍在(不自动清理用户数据)
    EXPECT_TRUE(fs::exists(fs::path(tmp.path()) / "abcdef0123456789" / "workspace.json"));
}

// 场景: workspace.json 缺 name 字段 → fallback 到 default_workspace_name(cwd)
TEST(WorkspaceRegistry, ScanMissingNameFallsBackToBasename) {
    TmpProjectsDir tmp;
    fs::create_directories(fs::path(tmp.path()) / "1111111111111111");
    {
        std::ofstream ofs((fs::path(tmp.path()) / "1111111111111111" / "workspace.json").string());
        ofs << R"({"cwd": "/home/u/code/projx"})"; // 缺 name
    } // ofs 析构 → 关闭 + 刷盘

    WorkspaceRegistry r;
    r.scan(tmp.path());
    auto v = r.list();
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0].name, "projx");
}

// 场景: register_new 给新 cwd 创建条目 + 写 workspace.json
TEST(WorkspaceRegistry, RegisterNewCreatesEntryAndFile) {
    TmpProjectsDir tmp;
    WorkspaceRegistry r;
    auto m = r.register_new(tmp.path(), "/home/u/test-proj");
    EXPECT_EQ(m.cwd, "/home/u/test-proj");
    EXPECT_EQ(m.name, "test-proj"); // 默认命名
    EXPECT_EQ(m.hash, compute_cwd_hash("/home/u/test-proj"));

    // 文件落地
    EXPECT_TRUE(fs::exists(fs::path(tmp.path()) / m.hash / "workspace.json"));

    // list 包含新条目
    EXPECT_EQ(r.list().size(), 1u);
}

// 场景: register_new 第二次同 cwd → 返回已有 meta,不重写文件
TEST(WorkspaceRegistry, RegisterNewIdempotent) {
    TmpProjectsDir tmp;
    WorkspaceRegistry r;
    auto m1 = r.register_new(tmp.path(), "/home/u/dup");
    // 改一下 name 模拟用户已经 set_name 过
    r.set_name(tmp.path(), m1.hash, "renamed");
    auto m2 = r.register_new(tmp.path(), "/home/u/dup");
    EXPECT_EQ(m2.hash, m1.hash);
    EXPECT_EQ(m2.name, "renamed"); // 没被默认名覆盖
    EXPECT_EQ(r.list().size(), 1u);
}

// 场景: set_name 拒空,且不动磁盘
TEST(WorkspaceRegistry, SetNameRejectsEmpty) {
    TmpProjectsDir tmp;
    WorkspaceRegistry r;
    auto m = r.register_new(tmp.path(), "/home/u/x");
    EXPECT_FALSE(r.set_name(tmp.path(), m.hash, ""));
    auto cur = r.get(m.hash);
    ASSERT_TRUE(cur.has_value());
    EXPECT_EQ(cur->name, "x"); // 仍是默认名
}

// 场景: set_name 未知 hash 报错
TEST(WorkspaceRegistry, SetNameUnknownHash) {
    TmpProjectsDir tmp;
    WorkspaceRegistry r;
    EXPECT_FALSE(r.set_name(tmp.path(), "nonexistent", "x"));
}

// 场景: set_name 持久化往返(写盘后重新 scan 仍是新名)
TEST(WorkspaceRegistry, SetNamePersistsAcrossScan) {
    TmpProjectsDir tmp;
    {
        WorkspaceRegistry r;
        auto m = r.register_new(tmp.path(), "/home/u/q");
        EXPECT_TRUE(r.set_name(tmp.path(), m.hash, "renamed-q"));
    }
    WorkspaceRegistry r2;
    r2.scan(tmp.path());
    auto v = r2.list();
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0].name, "renamed-q");
}

// 场景: 并发 set_name(同一 hash 多线程)不让磁盘留半截 / 内存出现损坏。
// 这条主要验证 mutex 没死锁 + atomic_write 不撞文件。
TEST(WorkspaceRegistry, ConcurrentSetNameDoesNotCorrupt) {
    TmpProjectsDir tmp;
    WorkspaceRegistry r;
    auto m = r.register_new(tmp.path(), "/home/u/concurrent");

    constexpr int kThreads = 8;
    std::vector<std::thread> ts;
    for (int i = 0; i < kThreads; ++i) {
        ts.emplace_back([&, i] {
            r.set_name(tmp.path(), m.hash, "name-" + std::to_string(i));
        });
    }
    for (auto& t : ts) t.join();

    // 内存里 name 应该是 8 个值之一,且 list 仍只有 1 条
    auto cur = r.get(m.hash);
    ASSERT_TRUE(cur.has_value());
    EXPECT_EQ(r.list().size(), 1u);

    // 文件应可解析(没残留半截 JSON)
    std::ifstream ifs((fs::path(tmp.path()) / m.hash / "workspace.json").string());
    ASSERT_TRUE(ifs.is_open());
    std::stringstream buf; buf << ifs.rdbuf();
    auto j = nlohmann::json::parse(buf.str()); // 抛异常即测试失败
    EXPECT_TRUE(j.contains("name"));
    EXPECT_TRUE(j["name"].is_string());
}

// 场景: get 不存在的 hash 返回 nullopt
TEST(WorkspaceRegistry, GetMissing) {
    WorkspaceRegistry r;
    EXPECT_FALSE(r.get("nope").has_value());
}
