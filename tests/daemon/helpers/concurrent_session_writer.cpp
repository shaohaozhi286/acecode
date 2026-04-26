// 测试辅助 binary: 在子进程里调用 SessionManager 写一条消息后退出。
// 用于 tests/daemon/concurrent_session_test.cpp 里 std::system() 起两份子进程
// 验证 daemon + TUI 同 cwd 不撞文件(对应 openspec add-web-daemon 任务 13.7)。
//
// 设计上**故意**不放进 acecode_unit_tests:
//   - 它是独立 main(),不是 GoogleTest 用例
//   - 它做的事是"写一条 session 消息 + 退出",有副作用,不能与其它测试同实例跑
//
// Argv 协议: <fake_home> <cwd> <content>
//   fake_home: 注入到 HOME / USERPROFILE,让 ~/.acecode 落到隔离目录,
//              避免污染开发机真实的 ~/.acecode/projects/
//   cwd:       用作 SessionManager::start_session(cwd, ...),决定 project_hash
//   content:   写入 user 消息的内容,用于测试侧验证内容不串号

#include "session/session_manager.hpp"
#include "session/session_storage.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#ifdef _WIN32
#  include <stdlib.h>
#endif

namespace {

void set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, /*overwrite=*/1);
#endif
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "usage: " << (argc > 0 ? argv[0] : "writer")
                  << " <fake_home> <cwd> <content>\n";
        return 99;
    }

    const std::string fake_home = argv[1];
    const std::string cwd       = argv[2];
    const std::string content   = argv[3];

    // 关键: 让 get_acecode_dir() 解析到 fake_home/.acecode,而不是真实用户目录。
    set_env("HOME", fake_home.c_str());
    set_env("USERPROFILE", fake_home.c_str());

    acecode::SessionManager mgr;
    mgr.start_session(cwd, "test-provider", "test-model");

    acecode::ChatMessage msg;
    msg.role = "user";
    msg.content = content;
    mgr.on_message(msg);
    mgr.finalize();

    // 输出 session_id 给父进程读(可选验证用)
    std::cout << mgr.current_session_id() << "\n";
    return 0;
}
