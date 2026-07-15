#include "desktop_restart.hpp"

#include "../utils/utf8_path.hpp"

#include <filesystem>
#include <cstdint>
#include <system_error>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <cerrno>
#  include <cstring>
#  include <fcntl.h>
#  include <sys/wait.h>
#  include <unistd.h>
#  ifdef __APPLE__
#    include <mach-o/dyld.h>
#  endif
#endif

namespace fs = std::filesystem;

namespace acecode::desktop {

fs::path current_desktop_executable_path() {
#ifdef _WIN32
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD size = static_cast<DWORD>(buffer.size());
        const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(), size);
        if (length == 0) return {};
        if (length < size - 1) return fs::path(std::wstring(buffer.data(), length));
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    uint32_t size = 1024;
    std::vector<char> buffer(size);
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        buffer.assign(static_cast<std::size_t>(size) + 1, '\0');
        if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    }
    return fs::path(buffer.data());
#else
    std::vector<char> buffer(4096);
    for (;;) {
        const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length < 0) return {};
        if (static_cast<std::size_t>(length) < buffer.size()) {
            return fs::path(std::string(buffer.data(), static_cast<std::size_t>(length)));
        }
        buffer.resize(buffer.size() * 2);
    }
#endif
}

DesktopRestartPreflight validate_desktop_restart_target(const fs::path& executable) {
    DesktopRestartPreflight result;
    if (executable.empty()) {
        result.error = "desktop executable path is empty";
        return result;
    }

    std::error_code ec;
    if (!fs::exists(executable, ec) || ec) {
        result.error = "desktop executable does not exist: " + acecode::path_to_utf8(executable);
        return result;
    }
    if (!fs::is_regular_file(executable, ec) || ec) {
        result.error = "desktop executable is not a regular file: " + acecode::path_to_utf8(executable);
        return result;
    }
#ifndef _WIN32
    if (::access(executable.c_str(), X_OK) != 0) {
        result.error = "desktop executable is not executable: " + acecode::path_to_utf8(executable);
        return result;
    }
#endif
    result.ok = true;
    return result;
}

bool launch_desktop_replacement(const fs::path& executable, std::string* error) {
    const auto preflight = validate_desktop_restart_target(executable);
    if (!preflight.ok) {
        if (error) *error = preflight.error;
        return false;
    }

#ifdef _WIN32
    std::wstring application = executable.native();
    std::wstring command_line = L"\"" + application + L"\"";
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const fs::path parent = executable.parent_path();
    const BOOL created = ::CreateProcessW(
        application.c_str(), mutable_command.data(), nullptr, nullptr, FALSE, 0,
        nullptr, parent.empty() ? nullptr : parent.c_str(), &startup, &process);
    if (!created) {
        if (error) {
            *error = "CreateProcessW failed with error " +
                     std::to_string(static_cast<unsigned long>(::GetLastError()));
        }
        return false;
    }
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return true;
#else
    int error_pipe[2] = {-1, -1};
    if (::pipe(error_pipe) != 0) {
        if (error) *error = std::string("pipe failed: ") + std::strerror(errno);
        return false;
    }
    if (::fcntl(error_pipe[1], F_SETFD, FD_CLOEXEC) != 0) {
        const int saved_errno = errno;
        ::close(error_pipe[0]);
        ::close(error_pipe[1]);
        if (error) *error = std::string("fcntl failed: ") + std::strerror(saved_errno);
        return false;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        const int saved_errno = errno;
        ::close(error_pipe[0]);
        ::close(error_pipe[1]);
        if (error) *error = std::string("fork failed: ") + std::strerror(saved_errno);
        return false;
    }
    if (pid == 0) {
        ::close(error_pipe[0]);
        ::setsid();
        const std::string native = acecode::path_to_utf8(executable);
        ::execl(native.c_str(), native.c_str(), static_cast<char*>(nullptr));
        const int saved_errno = errno;
        (void)::write(error_pipe[1], &saved_errno, sizeof(saved_errno));
        ::_exit(127);
    }

    ::close(error_pipe[1]);
    int child_errno = 0;
    ssize_t bytes = -1;
    do {
        bytes = ::read(error_pipe[0], &child_errno, sizeof(child_errno));
    } while (bytes < 0 && errno == EINTR);
    const int read_errno = errno;
    ::close(error_pipe[0]);
    if (bytes < 0) {
        if (error) *error = std::string("restart handshake failed: ") + std::strerror(read_errno);
        return false;
    }
    if (bytes > 0) {
        (void)::waitpid(pid, nullptr, 0);
        if (error) *error = std::string("exec failed: ") + std::strerror(child_errno);
        return false;
    }
    return true;
#endif
}

} // namespace acecode::desktop
