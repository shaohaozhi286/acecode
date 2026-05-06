#include "paths.hpp"

#include "logger.hpp"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

namespace acecode {

std::string expand_path(const std::string& raw) {
    if (raw.empty()) return raw;

    std::string result;
    result.reserve(raw.size());
    size_t i = 0;

    // ${VAR} substitution first so later ~ expansion sees final text.
    while (i < raw.size()) {
        if (raw[i] == '$' && i + 1 < raw.size() && raw[i + 1] == '{') {
            size_t end = raw.find('}', i + 2);
            if (end == std::string::npos) {
                result.push_back(raw[i++]);
                continue;
            }
            std::string var_name = raw.substr(i + 2, end - (i + 2));
            const char* val = std::getenv(var_name.c_str());
            if (val) {
                result.append(val);
            } else {
                result.append(raw, i, end + 1 - i); // leave ${VAR} untouched
            }
            i = end + 1;
        } else {
            result.push_back(raw[i++]);
        }
    }

    if (!result.empty() && result.front() == '~') {
#ifdef _WIN32
        const char* home = std::getenv("USERPROFILE");
#else
        const char* home = std::getenv("HOME");
#endif
        if (home && *home) {
            result = std::string(home) + result.substr(1);
        }
    }

    return result;
}

std::vector<std::string> get_project_dirs_up_to_home(const std::string& cwd) {
    namespace fs = std::filesystem;
    std::vector<std::string> dirs;
    if (cwd.empty()) return dirs;

    std::error_code ec;
    fs::path abs = fs::weakly_canonical(fs::path(cwd), ec);
    if (ec || abs.empty()) abs = fs::path(cwd);

    fs::path home_path;
#ifdef _WIN32
    const char* home_env = std::getenv("USERPROFILE");
#else
    const char* home_env = std::getenv("HOME");
#endif
    if (home_env && *home_env) {
        std::error_code hec;
        home_path = fs::weakly_canonical(fs::path(home_env), hec);
        if (hec) home_path = fs::path(home_env);
    }

    // Walk up from cwd; stop at/above HOME (the user-global root is added
    // separately) or once we hit a filesystem root. Deepest first.
    fs::path cur = abs;
    while (true) {
        if (!home_path.empty() && cur == home_path) break;
        dirs.push_back(cur.string());
        fs::path parent = cur.parent_path();
        if (parent == cur) break;
        cur = parent;
    }
    return dirs;
}

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
