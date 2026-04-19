// POSIX implementation of the daemon process abstraction. The whole file is
// guarded so it compiles to nothing on Windows; src/daemon/platform_windows.cpp
// is the symmetric file.
#ifndef _WIN32

#include "platform.hpp"

#include "../utils/logger.hpp"

#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#  include <mach-o/dyld.h>
#  include <climits>
#elif defined(__linux__)
#  include <climits>
#endif

namespace acecode::daemon {

pid_t_compat current_pid() { return static_cast<pid_t_compat>(::getpid()); }

pid_t_compat spawn_detached(const std::vector<std::string>& argv) {
    if (argv.empty()) return 0;

    pid_t first = ::fork();
    if (first < 0) {
        LOG_ERROR(std::string("[daemon] fork failed: ") + std::strerror(errno));
        return 0;
    }
    if (first > 0) {
        // Parent of intermediate: reap intermediate, then return its grandchild
        // pid by reading stdout pipe.
        int status = 0;
        ::waitpid(first, &status, 0);
        // We don't have a backchannel for the grandchild's pid here; callers
        // should rely on `~/.acecode/run/daemon.pid` written by the worker
        // itself. Return a sentinel non-zero so the caller knows fork chain
        // succeeded.
        return -1;
    }

    // Intermediate: detach and fork again so the worker reparents to init.
    if (::setsid() < 0) {
        LOG_ERROR(std::string("[daemon] setsid failed: ") + std::strerror(errno));
        ::_exit(1);
    }
    pid_t second = ::fork();
    if (second < 0) {
        ::_exit(1);
    }
    if (second > 0) {
        // Intermediate exits, worker continues alone.
        ::_exit(0);
    }

    // Worker: close stdio, redirect to /dev/null.
    int fd = ::open("/dev/null", O_RDWR);
    if (fd >= 0) {
        ::dup2(fd, STDIN_FILENO);
        ::dup2(fd, STDOUT_FILENO);
        ::dup2(fd, STDERR_FILENO);
        if (fd > 2) ::close(fd);
    }

    // Build argv array. Mutable copy because execv expects char* const[].
    std::vector<std::string> args = argv;
    std::vector<char*> c_args;
    c_args.reserve(args.size() + 1);
    for (auto& s : args) c_args.push_back(s.data());
    c_args.push_back(nullptr);

    ::execv(args[0].c_str(), c_args.data());
    // execv only returns on failure.
    LOG_ERROR(std::string("[daemon] execv failed: ") + std::strerror(errno));
    ::_exit(127);
}

bool is_pid_alive(pid_t_compat pid) {
    if (pid <= 0) return false;
    // Signal 0 performs no action but does the existence + permission check.
    if (::kill(static_cast<::pid_t>(pid), 0) == 0) return true;
    return errno == EPERM; // exists but not ours; still counts as alive
}

bool terminate_pid(pid_t_compat pid, int wait_ms) {
    if (pid <= 0) return true;
    if (!is_pid_alive(pid)) return true;
    if (::kill(static_cast<::pid_t>(pid), SIGTERM) != 0 && errno != ESRCH) {
        LOG_WARN(std::string("[daemon] SIGTERM failed: ") + std::strerror(errno));
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!is_pid_alive(pid)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (is_pid_alive(pid)) {
        ::kill(static_cast<::pid_t>(pid), SIGKILL);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return !is_pid_alive(pid);
}

std::string current_executable_path() {
#if defined(__APPLE__)
    char buf[PATH_MAX];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) return buf;
    return {};
#elif defined(__linux__)
    char buf[PATH_MAX];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) { buf[n] = '\0'; return buf; }
    return {};
#else
    return {};
#endif
}

} // namespace acecode::daemon

#endif // !_WIN32
