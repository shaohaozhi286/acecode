// Windows implementation of the daemon process abstraction. Symmetric with
// src/daemon/platform_posix.cpp; the entire file is guarded so it compiles
// to nothing on POSIX.
#ifdef _WIN32

#include "platform.hpp"

#include "../utils/logger.hpp"

#include <chrono>
#include <sstream>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace acecode::daemon {

namespace {

// Quote one argv element per the CommandLineToArgvW rules. We need this
// because CreateProcess takes a single command line string, not an argv array.
std::string quote_arg(const std::string& s) {
    bool need_quotes = s.empty() || s.find_first_of(" \t\"") != std::string::npos;
    if (!need_quotes) return s;

    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    int backslashes = 0;
    for (char c : s) {
        if (c == '\\') {
            ++backslashes;
        } else if (c == '"') {
            // Escape all backslashes preceding the quote, then the quote.
            for (int i = 0; i < backslashes * 2 + 1; ++i) out.push_back('\\');
            out.push_back('"');
            backslashes = 0;
        } else {
            for (int i = 0; i < backslashes; ++i) out.push_back('\\');
            backslashes = 0;
            out.push_back(c);
        }
    }
    // Trailing backslashes: double them so the closing quote isn't escaped.
    for (int i = 0; i < backslashes * 2; ++i) out.push_back('\\');
    out.push_back('"');
    return out;
}

std::string build_command_line(const std::vector<std::string>& argv) {
    std::ostringstream oss;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i) oss << ' ';
        oss << quote_arg(argv[i]);
    }
    return oss.str();
}

} // namespace

pid_t_compat current_pid() {
    return static_cast<pid_t_compat>(::GetCurrentProcessId());
}

pid_t_compat spawn_detached(const std::vector<std::string>& argv) {
    if (argv.empty()) return 0;

    std::string cmdline = build_command_line(argv);
    std::vector<char> cmd_buf(cmdline.begin(), cmdline.end());
    cmd_buf.push_back('\0');

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    DWORD flags = DETACHED_PROCESS | CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP;
    BOOL ok = ::CreateProcessA(
        argv[0].c_str(),  // application name (absolute path)
        cmd_buf.data(),   // mutable command line
        nullptr,          // process security
        nullptr,          // thread security
        FALSE,            // do not inherit handles
        flags,
        nullptr,          // inherit environment
        nullptr,          // inherit cwd
        &si,
        &pi);
    if (!ok) {
        DWORD err = ::GetLastError();
        LOG_ERROR("[daemon] CreateProcess failed, GLE=" + std::to_string(err));
        return 0;
    }
    DWORD child_pid = pi.dwProcessId;
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return static_cast<pid_t_compat>(child_pid);
}

bool is_pid_alive(pid_t_compat pid) {
    if (pid <= 0) return false;
    HANDLE h = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                             FALSE, static_cast<DWORD>(pid));
    if (!h) return false;
    DWORD wait = ::WaitForSingleObject(h, 0);
    ::CloseHandle(h);
    return wait == WAIT_TIMEOUT; // signaled means already exited
}

bool terminate_pid(pid_t_compat pid, int wait_ms) {
    if (pid <= 0) return true;
    if (!is_pid_alive(pid)) return true;

    // Try a graceful break first. CTRL_BREAK_EVENT requires the target to be
    // in the same console process group; with CREATE_NEW_PROCESS_GROUP at
    // spawn time, that holds for our worker.
    ::GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, static_cast<DWORD>(pid));

    HANDLE h = ::OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    if (!h) return !is_pid_alive(pid);

    DWORD wait = ::WaitForSingleObject(h, static_cast<DWORD>(wait_ms));
    bool exited = (wait == WAIT_OBJECT_0);
    if (!exited) {
        ::TerminateProcess(h, 1);
        ::WaitForSingleObject(h, 2000);
        exited = (::WaitForSingleObject(h, 0) == WAIT_OBJECT_0);
    }
    ::CloseHandle(h);
    // small grace period before re-checking via OpenProcess
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return !is_pid_alive(pid);
}

std::string current_executable_path() {
    char buf[MAX_PATH];
    DWORD n = ::GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return {};
    return std::string(buf, buf + n);
}

} // namespace acecode::daemon

#endif // _WIN32
