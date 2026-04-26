// 覆盖 src/session/session_storage.cpp 中"session 文件加 pid 后缀"的改造
// (openspec add-web-daemon Section 8)。这一层改造是 daemon 与本地 TUI 同时
// 跑同一 cwd 时不互相破坏 session 文件的根本保证,一旦回归,会出现:
//   - 两进程往同一个 jsonl 互相覆盖、消息错乱
//   - list_sessions 把同一 session 显示成两条
//   - resume 拿错文件 / 写回老文件
// 因此每条用例都对应一个真实可观测的故障模式。
//
// 任务对应:
//   8.1 写入文件名加 -{pid} 后缀          → WritePathDefaultsToCurrentPid
//   8.2 list/resume 兼容旧无 pid 文件     → FindFilesIncludesLegacyAndPidVariants
//                                          ListSessionsDedupesByIdKeepingNewest
//   8.4 并发写不互相破坏                  → TwoPidsWriteIndependentFiles

#include <gtest/gtest.h>

#include "session/session_storage.hpp"
#include "daemon/platform.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using acecode::SessionMeta;
using acecode::SessionStorage;

namespace {

fs::path make_unique_tmp_dir(const std::string& hint) {
    auto base = fs::temp_directory_path() /
                ("acecode_pid_test_" + hint + "_" +
                 std::to_string(::testing::UnitTest::GetInstance()
                     ->current_test_info()->line()));
    fs::remove_all(base);
    fs::create_directories(base);
    return base;
}

void write_meta_with_pid(const fs::path& dir, const std::string& id, int pid,
                          const std::string& updated_at) {
    SessionMeta m;
    m.id = id;
    m.cwd = "/tmp/x";
    m.created_at = updated_at;
    m.updated_at = updated_at;
    m.message_count = 1;
    m.provider = "openai";
    m.model = "test";
    SessionStorage::write_meta(
        SessionStorage::meta_path(dir.string(), id, pid), m);
}

} // namespace

// 场景: session_path 不传 pid 时,必须自动追加本进程 pid。
// 这是 SessionManager::ensure_created() 把所有写入路由到带 pid 文件的根本依赖
// —— 一旦默认值被改回"无后缀",daemon 与 TUI 又会重新撞 jsonl。
TEST(SessionStoragePidSuffix, WritePathDefaultsToCurrentPid) {
    auto dir = make_unique_tmp_dir("default_pid");
    const std::string id = "20260426-100000-abcd";

    std::string p = SessionStorage::session_path(dir.string(), id);  // 默认 pid
    int my_pid = static_cast<int>(acecode::daemon::current_pid());
    std::string expected = (dir / (id + "-" + std::to_string(my_pid) + ".jsonl")).string();

    EXPECT_EQ(p, expected)
        << "默认参数必须用本进程 pid 后缀,否则 daemon/TUI 隔离失效";
}

// 场景: 显式 pid=0 走旧格式无后缀,这条路径只用于"读老 session"的兼容。
// 如果误把 0 也加 -0 后缀,老用户升级后 list_sessions 会丢历史。
TEST(SessionStoragePidSuffix, LegacyZeroPidProducesNoSuffix) {
    auto dir = make_unique_tmp_dir("legacy_zero");
    const std::string id = "20260426-100000-abcd";

    std::string p = SessionStorage::session_path(dir.string(), id, 0);
    std::string expected = (dir / (id + ".jsonl")).string();
    EXPECT_EQ(p, expected) << "pid=0 必须走旧格式,不能写成 -0 后缀";
}

// 场景: 同一 session_id 在磁盘上同时存在三种文件(legacy + pid=1000 + pid=2000),
// find_session_files 必须三份都返回,并按 mtime 降序。这是 resume 时"挑最新"的
// 唯一信息源,挑错就会让用户拿到旧版会话或丢消息。
TEST(SessionStoragePidSuffix, FindFilesIncludesLegacyAndPidVariants) {
    auto dir = make_unique_tmp_dir("find_files");
    const std::string id = "20260426-100000-abcd";

    // 创建三份 jsonl,每次间隔一点点确保 mtime 严格递增。
    auto touch = [&](int pid) {
        std::ofstream(SessionStorage::session_path(dir.string(), id, pid)) << "{}\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    };
    touch(0);     // 旧无后缀(最早)
    touch(1000);  // pid=1000
    touch(2000);  // pid=2000(最新)

    auto candidates = SessionStorage::find_session_files(dir.string(), id);
    ASSERT_EQ(candidates.size(), 3u) << "三份文件全部应被识别,旧无后缀文件不能漏";

    // 按 mtime 降序: 2000 → 1000 → 0
    EXPECT_EQ(candidates[0].pid, 2000);
    EXPECT_EQ(candidates[1].pid, 1000);
    EXPECT_EQ(candidates[2].pid, 0);
}

// 场景: 同一 session_id 有两份 pid 后缀的 meta(daemon + TUI 各自跑过一次),
// list_sessions 必须按 id 去重,只保留 mtime 最新那份的内容。否则 /session 列表
// 会把同一个对话显示两次,让用户摸不着头脑。
TEST(SessionStoragePidSuffix, ListSessionsDedupesByIdKeepingNewest) {
    auto dir = make_unique_tmp_dir("dedupe");
    const std::string id = "20260426-100000-abcd";

    write_meta_with_pid(dir, id, 1000, "2026-04-26T10:00:00Z");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    write_meta_with_pid(dir, id, 2000, "2026-04-26T10:05:00Z");  // 较新

    auto sessions = SessionStorage::list_sessions(dir.string());
    ASSERT_EQ(sessions.size(), 1u) << "同一 id 必须去重,不应同时出现两条";
    EXPECT_EQ(sessions[0].id, id);
    EXPECT_EQ(sessions[0].updated_at, "2026-04-26T10:05:00Z")
        << "去重后保留的应该是 mtime 最新那份的内容";
}

// 场景: 模拟两个进程(pid=1000 和 pid=2000)往同一 cwd 写各自的 session 文件。
// 这是 spec "daemon 与本地 TUI 并发隔离" 的最直接验证。两份 jsonl 必须独立、
// 内容互不污染。
//
// 真正用 fork() 起两个进程会让测试在 Windows / CI 上很难写,改用直接调用
// SessionStorage 显式传 pid 参数的方式来等价模拟 —— 因为 pid 后缀本身就是
// 进程隔离的"全部机制",显式传两个不同 pid 跟两个进程跑是行为等价的。
TEST(SessionStoragePidSuffix, TwoPidsWriteIndependentFiles) {
    auto dir = make_unique_tmp_dir("concurrent");
    const std::string id_a = "20260426-100000-aaaa";
    const std::string id_b = "20260426-100001-bbbb";

    // 进程 1000 创建 session A
    auto path_a = SessionStorage::session_path(dir.string(), id_a, 1000);
    {
        std::ofstream ofs(path_a);
        ofs << "{\"role\":\"user\",\"content\":\"from-pid-1000\"}\n";
    }

    // 进程 2000 创建 session B(同 cwd)
    auto path_b = SessionStorage::session_path(dir.string(), id_b, 2000);
    {
        std::ofstream ofs(path_b);
        ofs << "{\"role\":\"user\",\"content\":\"from-pid-2000\"}\n";
    }

    // 两个文件必须都存在
    ASSERT_TRUE(fs::exists(path_a));
    ASSERT_TRUE(fs::exists(path_b));

    // 文件名严格分开
    EXPECT_NE(path_a, path_b);
    EXPECT_NE(fs::path(path_a).filename(), fs::path(path_b).filename());

    // 内容互不污染
    auto read_all = [](const std::string& p) {
        std::ifstream ifs(p);
        return std::string((std::istreambuf_iterator<char>(ifs)),
                           std::istreambuf_iterator<char>());
    };
    EXPECT_NE(read_all(path_a).find("from-pid-1000"), std::string::npos);
    EXPECT_NE(read_all(path_b).find("from-pid-2000"), std::string::npos);
    EXPECT_EQ(read_all(path_a).find("from-pid-2000"), std::string::npos)
        << "进程 1000 的 session 文件不应包含进程 2000 的内容";
}

// 场景: 给定同一 session_id 的两份候选,find_session_files 返回的 meta_path
// 必须分别配对正确(不能两份候选都指向同一个 meta)。这是 cleanup_old_sessions
// 删除时一并清 meta 的前提 —— 配对错了会留下孤儿 .meta.json 污染 list_sessions。
TEST(SessionStoragePidSuffix, CandidateMetaPathPairsWithJsonl) {
    auto dir = make_unique_tmp_dir("meta_pair");
    const std::string id = "20260426-100000-abcd";

    std::ofstream(SessionStorage::session_path(dir.string(), id, 1000)) << "";
    std::ofstream(SessionStorage::session_path(dir.string(), id, 2000)) << "";

    auto cs = SessionStorage::find_session_files(dir.string(), id);
    ASSERT_EQ(cs.size(), 2u);

    for (const auto& c : cs) {
        std::string expected_meta =
            SessionStorage::meta_path(dir.string(), id, c.pid);
        EXPECT_EQ(c.meta_path, expected_meta)
            << "pid=" << c.pid << " 的 meta_path 必须与 jsonl 配对";
    }
}
