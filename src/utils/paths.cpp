#include "paths.hpp"

#include "logger.hpp"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>

namespace acecode {

namespace {

// 进程级单例。RunMode 用 atomic 是因为 set_run_mode 可能在 ServiceMain 早期由
// SCM 线程调,而 get_run_mode 在 worker 线程读 — 不加保护理论上算 data race。
std::atomic<RunMode> g_mode{RunMode::User};
std::atomic<bool>    g_set_once{false};

// run_dir override:string 不能 atomic,加 mutex。daemon 启动早期 set 一次,
// 之后所有 get_run_dir() 调都读这个值;频率不高,锁开销可忽略。
std::mutex  g_run_dir_mu;
std::string g_run_dir_override;

} // namespace

void set_run_mode(RunMode mode) {
    bool expected = false;
    if (g_set_once.compare_exchange_strong(expected, true)) {
        g_mode.store(mode);
        return;
    }
    // 二次调用 — 不改值,只警告。pre-logger-init 时这条 LOG_WARN 会被吞掉,
    // 但生产代码里调到这里几乎肯定是 bug,测试会断到。
    int cur = static_cast<int>(g_mode.load());
    int req = static_cast<int>(mode);
    LOG_WARN(std::string("set_run_mode called more than once; ignoring (current=")
             + std::to_string(cur) + " requested=" + std::to_string(req) + ")");
}

RunMode get_run_mode() {
    return g_mode.load();
}

std::string resolve_data_dir(RunMode mode) {
    namespace fs = std::filesystem;

#if defined(_WIN32)
    if (mode == RunMode::Service) {
        const char* progdata = std::getenv("PROGRAMDATA");
        std::string base = progdata ? progdata : "C:\\ProgramData";
        return (fs::path(base) / "acecode").string();
    }
    // RunMode::User — 与历史 get_acecode_dir() 行为完全一致
    if (const char* userprofile = std::getenv("USERPROFILE")) {
        return (fs::path(userprofile) / ".acecode").string();
    }
    const char* drive = std::getenv("HOMEDRIVE");
    const char* path  = std::getenv("HOMEPATH");
    if (drive && path) {
        return (fs::path(std::string(drive) + path) / ".acecode").string();
    }
    return (fs::path(".") / ".acecode").string();

#elif defined(__APPLE__)
    if (mode == RunMode::Service) {
        return "/Library/Application Support/acecode";
    }
    const char* home = std::getenv("HOME");
    return (fs::path(home ? home : ".") / ".acecode").string();

#else // Linux 或其他 POSIX
    if (mode == RunMode::Service) {
        return "/var/lib/acecode";
    }
    const char* home = std::getenv("HOME");
    return (fs::path(home ? home : ".") / ".acecode").string();
#endif
}

void set_run_dir_override(const std::string& path) {
    std::lock_guard<std::mutex> lk(g_run_dir_mu);
    g_run_dir_override = path;
}

std::string get_run_dir_override() {
    std::lock_guard<std::mutex> lk(g_run_dir_mu);
    return g_run_dir_override;
}

RunMode override_run_mode_for_test(RunMode mode) {
    RunMode prev = g_mode.load();
    g_mode.store(mode);
    return prev;
}

void reset_run_mode_for_test() {
    g_mode.store(RunMode::User);
    g_set_once.store(false);
    {
        std::lock_guard<std::mutex> lk(g_run_dir_mu);
        g_run_dir_override.clear();
    }
}

} // namespace acecode
