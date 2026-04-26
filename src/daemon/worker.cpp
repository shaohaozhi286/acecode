#include "worker.hpp"

#include "guid.hpp"
#include "heartbeat.hpp"
#include "platform.hpp"
#include "runtime_files.hpp"
#include "../utils/logger.hpp"
#include "../utils/token.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace acecode::daemon {

namespace {

// 终止信号 → 唤醒主循环退出。POSIX 与 Windows 各有一套。
std::mutex              g_term_mu;
std::condition_variable g_term_cv;
std::atomic<bool>       g_term_requested{false};

void request_terminate() {
    g_term_requested.store(true);
    g_term_cv.notify_all();
}

#ifdef _WIN32
BOOL WINAPI win_console_handler(DWORD ctrl) {
    switch (ctrl) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            request_terminate();
            return TRUE;
        default:
            return FALSE;
    }
}
#else
extern "C" void posix_term_handler(int /*signo*/) {
    request_terminate();
}
#endif

void install_term_handlers() {
#ifdef _WIN32
    ::SetConsoleCtrlHandler(win_console_handler, TRUE);
#else
    std::signal(SIGTERM, posix_term_handler);
    std::signal(SIGINT,  posix_term_handler);
#endif
}

} // namespace

std::string validate_can_start(const WorkerOptions& opts) {
    auto existing_guid = read_guid_file();
    auto existing_pid  = read_pid_file();

    if (opts.supervised) {
        // launcher 派 GUID 进来。如果磁盘已有 guid 但跟 launcher 派的不一致,
        // 视为另一个 launcher 已抢占了 daemon.guid,拒启。
        if (existing_guid.has_value() && !existing_guid->empty() &&
            *existing_guid != opts.guid) {
            return "another supervised worker already owns daemon.guid (expected="
                   + opts.guid + " actual=" + *existing_guid + ")";
        }
        return {};
    }

    // standalone: 若已有 guid + pid 文件,且 pid 仍存活,说明已有 daemon 跑。
    if (existing_guid.has_value() && existing_pid.has_value() &&
        is_pid_alive(*existing_pid)) {
        std::ostringstream oss;
        oss << "another daemon already running (pid=" << *existing_pid
            << " guid=" << *existing_guid << ")";
        return oss.str();
    }
    return {};
}

int run_worker(const WorkerOptions& opts, const AppConfig& cfg) {
    // 启动前安全校验
    if (cfg.web.bind != "127.0.0.1" && cfg.web.bind != "::1" && opts.dangerous) {
        std::cerr << "dangerous mode is loopback-only (web.bind="
                  << cfg.web.bind << ")\n";
        return 2;
    }

    auto reject = validate_can_start(opts);
    if (!reject.empty()) {
        std::cerr << "[daemon] refuse to start: " << reject << "\n";
        return 3;
    }

    ensure_run_dir();

    // GUID: supervised 用 launcher 派的;standalone 自己生成。
    std::string guid = opts.supervised ? opts.guid : generate_daemon_guid();
    std::int64_t pid = current_pid();

    // 写运行时产物。顺序: guid → pid → port → token,失败立刻退出。
    if (!write_guid_file(guid))           { std::cerr << "write guid failed\n"; return 4; }
    if (!write_pid_file(pid))             { std::cerr << "write pid failed\n"; return 4; }
    if (!write_port_file(cfg.web.port))   { std::cerr << "write port failed\n"; return 4; }

    auto token = acecode::generate_auth_token();
    if (token.empty() || !write_token(token)) {
        std::cerr << "write token failed\n";
        return 4;
    }

    {
        std::ostringstream oss;
        oss << "[daemon] worker started pid=" << pid
            << " guid=" << guid
            << " bind=" << cfg.web.bind << ":" << cfg.web.port
            << (opts.supervised ? " mode=supervised" : " mode=standalone");
        LOG_INFO(oss.str());
        if (opts.foreground) std::cerr << oss.str() << "\n";
    }

    // 心跳
    HeartbeatWriter heartbeat(pid, guid, cfg.daemon.heartbeat_interval_ms);
    heartbeat.start();

    install_term_handlers();

    // TODO(Section 9): 这里启动 Crow HTTP/WebSocket server。
    // 现阶段只是阻塞等终止信号 — 让 daemon start/stop/status 端到端可用。
    {
        std::unique_lock<std::mutex> lk(g_term_mu);
        g_term_cv.wait(lk, [] { return g_term_requested.load(); });
    }

    LOG_INFO("[daemon] worker shutting down");
    if (opts.foreground) std::cerr << "[daemon] shutting down\n";

    heartbeat.stop();
    cleanup_runtime_files();
    return 0;
}

} // namespace acecode::daemon
