// 端到端验证 daemon + 本地 TUI 同 cwd 不撞 session 文件(openspec add-web-daemon
// 任务 13.7)。spec 8 + 8.4 单元测试已经覆盖了 SessionStorage 层的文件名隔离;
// 这里再用真实 OS 子进程跑一遍,守住跨进程的最后一公里风险:
//   - fs::create_directories 在两进程并发下的行为
//   - 文件系统 rename / append 是不是真的不串
//   - 同一个 session_id 生成器在不同进程里碰撞概率(理论上 4 hex random 容易撞)
//
// 实现: 用 CMake 注入的 helper binary 路径,std::system() 顺序起两个子进程,
// 每个写一条带签名 content 的消息。验证:
//   1. 两个 jsonl 文件都存在
//   2. 文件名 pid 后缀不同
//   3. 内容只在各自文件出现,没串号

#include <gtest/gtest.h>

#include "session/session_storage.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path make_unique_tmp(const std::string& hint) {
    auto base = fs::temp_directory_path() /
                ("acecode_e2e_" + hint + "_" +
                 std::to_string(::testing::UnitTest::GetInstance()
                     ->current_test_info()->line()) + "_" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(base);
    fs::create_directories(base);
    return base;
}

// Windows 路径含空格 / 反斜杠时,std::system 需要外层再套一层引号。
// 用 ostringstream 自己拼,跨平台都行。
std::string quote_arg(const std::string& s) {
    std::string out = "\"";
    out += s;
    out += "\"";
    return out;
}

int run_writer(const std::string& fake_home,
                const std::string& cwd,
                const std::string& content) {
    std::ostringstream cmd;
#ifdef _WIN32
    // cmd.exe 处理嵌套引号的 quirk: 整个命令再外层包一层引号
    cmd << "\"";
#endif
    cmd << quote_arg(ACECODE_CONCURRENT_SESSION_WRITER_PATH)
        << " " << quote_arg(fake_home)
        << " " << quote_arg(cwd)
        << " " << quote_arg(content);
#ifdef _WIN32
    cmd << "\"";
#endif
    return std::system(cmd.str().c_str());
}

} // namespace

// 场景: 两个真实子进程在同一 cwd 各自写一条消息,产物必须是两份独立文件,
// pid 后缀不同,内容互不污染。spec 直接要求(daemon + TUI 并发隔离)。
TEST(SessionConcurrencyE2E, TwoRealSubprocessesProduceIndependentFiles) {
    auto fake_home = make_unique_tmp("home");
    auto cwd       = make_unique_tmp("cwd");

    int rc1 = run_writer(fake_home.string(), cwd.string(), "content-from-process-A");
    int rc2 = run_writer(fake_home.string(), cwd.string(), "content-from-process-B");
    ASSERT_EQ(rc1, 0) << "writer 子进程 A 应当正常退出";
    ASSERT_EQ(rc2, 0) << "writer 子进程 B 应当正常退出";

    // 计算 helper 那边落到的 project_dir(get_acecode_dir 用 fake_home)
    auto project_dir = fake_home / ".acecode" / "projects" /
                        acecode::SessionStorage::compute_project_hash(cwd.string());
    ASSERT_TRUE(fs::exists(project_dir))
        << "project_dir 应当被两个子进程之一 create_directories 出来: "
        << project_dir.string();

    // 收集所有 jsonl 文件名,按文件名匹配 -<pid>.jsonl 后缀
    std::regex re(R"(^\d{8}-\d{6}-[0-9a-f]{4}-(\d+)\.jsonl$)");
    std::vector<int> pids;
    std::vector<fs::path> jsonl_paths;
    for (const auto& e : fs::directory_iterator(project_dir)) {
        if (!e.is_regular_file()) continue;
        std::string fname = e.path().filename().string();
        std::smatch m;
        if (std::regex_match(fname, m, re)) {
            pids.push_back(std::stoi(m[1].str()));
            jsonl_paths.push_back(e.path());
        }
    }

    ASSERT_EQ(jsonl_paths.size(), 2u)
        << "两个子进程应当各自落一份带 pid 后缀的 jsonl,实际找到 "
        << jsonl_paths.size() << " 份";
    ASSERT_EQ(pids.size(), 2u);
    EXPECT_NE(pids[0], pids[1]) << "两子进程 pid 必须不同";

    // 内容互不污染: content-A 只能在一个文件里出现,content-B 同样
    int files_with_a = 0, files_with_b = 0;
    for (const auto& p : jsonl_paths) {
        std::ifstream ifs(p);
        std::string body((std::istreambuf_iterator<char>(ifs)),
                          std::istreambuf_iterator<char>());
        if (body.find("content-from-process-A") != std::string::npos) ++files_with_a;
        if (body.find("content-from-process-B") != std::string::npos) ++files_with_b;
    }
    EXPECT_EQ(files_with_a, 1) << "content-A 应只出现在一份文件中";
    EXPECT_EQ(files_with_b, 1) << "content-B 应只出现在一份文件中";

    // SessionStorage::list_sessions 必须把两条都看见(它按 id 去重,
    // 两个子进程的 session_id 不同,所以应该有 2 条)
    auto sessions = acecode::SessionStorage::list_sessions(project_dir.string());
    EXPECT_EQ(sessions.size(), 2u)
        << "list_sessions 应同时枚举两子进程产生的不同 session";

    // cleanup
    std::error_code ec;
    fs::remove_all(fake_home, ec);
    fs::remove_all(cwd, ec);
}
