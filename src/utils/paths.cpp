#include "paths.hpp"

#include "encoding.hpp"
#include "logger.hpp"
#include "utf8_path.hpp"

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
            std::string val = getenv_utf8(var_name.c_str());
            if (!val.empty()) {
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
        std::string home = getenv_utf8("USERPROFILE");
#else
        std::string home = getenv_utf8("HOME");
#endif
        if (!home.empty()) {
            result = home + result.substr(1);
        }
    }

    return result;
}

std::vector<std::string> get_project_dirs_up_to_home(const std::string& cwd) {
    namespace fs = std::filesystem;
    std::vector<std::string> dirs;
    if (cwd.empty()) return dirs;

    std::error_code ec;
    fs::path abs = fs::absolute(path_from_utf8(cwd), ec).lexically_normal();
    if (ec || abs.empty()) abs = path_from_utf8(cwd).lexically_normal();

    fs::path home_path;
#ifdef _WIN32
    std::string home_env = getenv_utf8("USERPROFILE");
#else
    std::string home_env = getenv_utf8("HOME");
#endif
    if (!home_env.empty()) {
        std::error_code hec;
        home_path = fs::absolute(path_from_utf8(home_env), hec).lexically_normal();
        if (hec) home_path = path_from_utf8(home_env).lexically_normal();
    }

    // Walk up from cwd; stop at/above HOME (the user-global root is added
    // separately) or once we hit a filesystem root. Deepest first.
    //
    // HOME 比较除了字面相等还做一次物理等价判定(fs::equivalent):subst /
    // 网络映射盘会让同一物理目录出现两个字面路径(如 N:\Users\shao ↔
    // C:\Users\shao)。cwd 走映射盘视图时若只做字符串比较,项目链会越过
    // HOME 一路冲到盘根,把 HOME 级目录(~/.claude 等)全部卷成"项目链",
    // 技能/项目指令的 project vs global 归类随之全错。
    fs::path cur = abs;
    while (true) {
        if (!home_path.empty()) {
            if (cur == home_path) break;
            std::error_code eqec;
            if (fs::equivalent(cur, home_path, eqec) && !eqec) break;
        }
        dirs.push_back(path_to_utf8(cur));
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
        std::string base = getenv_utf8("PROGRAMDATA");
        if (base.empty()) base = "C:\\ProgramData";
        return path_to_utf8(path_from_utf8(base) / "acecode");
    }
    // RunMode::User — 与历史 get_acecode_dir() 行为完全一致
    if (std::string userprofile = getenv_utf8("USERPROFILE"); !userprofile.empty()) {
        return path_to_utf8(path_from_utf8(userprofile) / ".acecode");
    }
    std::string drive = getenv_utf8("HOMEDRIVE");
    std::string path  = getenv_utf8("HOMEPATH");
    if (!drive.empty() && !path.empty()) {
        return path_to_utf8(path_from_utf8(drive + path) / ".acecode");
    }
    return path_to_utf8(path_from_utf8(".") / ".acecode");

#elif defined(__APPLE__)
    if (mode == RunMode::Service) {
        return "/Library/Application Support/acecode";
    }
    std::string home = getenv_utf8("HOME");
    return path_to_utf8(path_from_utf8(home.empty() ? "." : home) / ".acecode");

#else // Linux 或其他 POSIX
    if (mode == RunMode::Service) {
        return "/var/lib/acecode";
    }
    std::string home = getenv_utf8("HOME");
    return path_to_utf8(path_from_utf8(home.empty() ? "." : home) / ".acecode");
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
