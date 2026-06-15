#include "hook_runner.hpp"

#include "../utils/encoding.hpp"
#include "../utils/utf8_path.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace acecode {
namespace {

constexpr std::size_t kMaxCapturedOutputBytes = 64 * 1024;

long long elapsed_ms_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
}

void append_capped(std::string& out, const char* data, std::size_t len) {
    if (len == 0 || out.size() >= kMaxCapturedOutputBytes) return;
    std::size_t remaining = kMaxCapturedOutputBytes - out.size();
    std::size_t take = std::min(remaining, len);
    out.append(data, data + take);
    if (take < len && out.size() < kMaxCapturedOutputBytes + 32) {
        out.append("\n[hook output truncated]\n");
    }
}

std::vector<std::string> make_argv(const HookCommandSpec& command) {
    std::vector<std::string> argv;
    argv.reserve(command.args.size() + 1);
    argv.push_back(command.command);
    argv.insert(argv.end(), command.args.begin(), command.args.end());
    return argv;
}

bool has_path_separator(const std::string& value) {
    return value.find('/') != std::string::npos || value.find('\\') != std::string::npos;
}

std::filesystem::path current_executable_dir() {
    namespace fs = std::filesystem;
    std::error_code ec;
#ifdef _WIN32
    std::vector<wchar_t> buffer(MAX_PATH);
    DWORD n = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    while (n == buffer.size()) {
        buffer.resize(buffer.size() * 2);
        n = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    }
    if (n > 0) {
        fs::path p(std::wstring(buffer.data(), n));
        return fs::weakly_canonical(p.parent_path(), ec);
    }
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    if (size > 0) {
        std::vector<char> buffer(size + 1, '\0');
        if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
            fs::path p(std::string(buffer.data()));
            return fs::weakly_canonical(p.parent_path(), ec);
        }
    }
#else
    std::array<char, 4096> buffer{};
    ssize_t n = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (n > 0) {
        fs::path p(std::string(buffer.data(), static_cast<std::size_t>(n)));
        return fs::weakly_canonical(p.parent_path(), ec);
    }
#endif
    return {};
}

#ifdef _WIN32

void close_handle(HANDLE& h) {
    if (h && h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        h = nullptr;
    }
}

std::string windows_error_message(DWORD code) {
    LPWSTR buffer = nullptr;
    DWORD size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);
    std::wstring message = (size && buffer) ? std::wstring(buffer, size) : L"unknown error";
    if (buffer) LocalFree(buffer);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n')) {
        message.pop_back();
    }
    return wide_to_utf8(message);
}

std::wstring quote_windows_arg(const std::string& arg_utf8) {
    std::wstring arg = utf8_to_wide(arg_utf8);
    bool needs_quotes = arg.empty() || arg.find_first_of(L" \t\"") != std::wstring::npos;
    if (!needs_quotes) return arg;

    std::wstring out;
    out.reserve(arg.size() + 2);
    out.push_back(L'"');
    int backslashes = 0;
    for (wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
        } else if (ch == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
        } else {
            out.append(backslashes, L'\\');
            backslashes = 0;
            out.push_back(ch);
        }
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

std::wstring build_windows_command_line(const std::vector<std::string>& argv) {
    std::wstring out;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i) out.push_back(L' ');
        out += quote_windows_arg(argv[i]);
    }
    return out;
}

#else

void close_fd(int& fd) {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

#endif

} // namespace

std::string resolve_hook_command_path(const std::string& command) {
    if (command.empty()) return command;

    namespace fs = std::filesystem;
    fs::path native = path_from_utf8(command);
    if (native.is_absolute()) return command;

    fs::path exe_dir = current_executable_dir();
    if (exe_dir.empty()) return command;

    fs::path candidate = (exe_dir / native).lexically_normal();
    std::error_code ec;
    if (has_path_separator(command) || fs::exists(candidate, ec)) {
        return path_to_utf8(candidate);
    }
    return command;
}

HookProcessResult run_hook_process(const HookCommandSpec& command,
                                   const std::string& stdin_text,
                                   int timeout_ms,
                                   const std::string& cwd) {
    HookProcessResult result;
    auto started_at = std::chrono::steady_clock::now();
    auto finish = [&]() {
        result.duration_ms = elapsed_ms_since(started_at);
        result.stdout_text = ensure_utf8(result.stdout_text);
        result.stderr_text = ensure_utf8(result.stderr_text);
        result.output = result.stdout_text + result.stderr_text;
        result.error = ensure_utf8(result.error);
        return result;
    };

    std::vector<std::string> argv = make_argv(command);
    if (argv.empty() || argv[0].empty()) {
        result.error = "hook command is empty";
        return finish();
    }
    argv[0] = resolve_hook_command_path(argv[0]);

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE child_stdin_read = nullptr;
    HANDLE child_stdin_write = nullptr;
    HANDLE child_stdout_read = nullptr;
    HANDLE child_stdout_write = nullptr;
    HANDLE child_stderr_read = nullptr;
    HANDLE child_stderr_write = nullptr;

    auto cleanup = [&]() {
        close_handle(child_stdin_read);
        close_handle(child_stdin_write);
        close_handle(child_stdout_read);
        close_handle(child_stdout_write);
        close_handle(child_stderr_read);
        close_handle(child_stderr_write);
    };

    if (!CreatePipe(&child_stdin_read, &child_stdin_write, &sa, 0)) {
        result.error = "CreatePipe(stdin) failed: " + windows_error_message(GetLastError());
        return finish();
    }
    if (!SetHandleInformation(child_stdin_write, HANDLE_FLAG_INHERIT, 0)) {
        result.error = "SetHandleInformation(stdin) failed: " + windows_error_message(GetLastError());
        cleanup();
        return finish();
    }
    if (!CreatePipe(&child_stdout_read, &child_stdout_write, &sa, 0)) {
        result.error = "CreatePipe(stdout) failed: " + windows_error_message(GetLastError());
        cleanup();
        return finish();
    }
    if (!SetHandleInformation(child_stdout_read, HANDLE_FLAG_INHERIT, 0)) {
        result.error = "SetHandleInformation(stdout) failed: " + windows_error_message(GetLastError());
        cleanup();
        return finish();
    }
    if (!CreatePipe(&child_stderr_read, &child_stderr_write, &sa, 0)) {
        result.error = "CreatePipe(stderr) failed: " + windows_error_message(GetLastError());
        cleanup();
        return finish();
    }
    if (!SetHandleInformation(child_stderr_read, HANDLE_FLAG_INHERIT, 0)) {
        result.error = "SetHandleInformation(stderr) failed: " + windows_error_message(GetLastError());
        cleanup();
        return finish();
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = child_stdin_read;
    si.hStdOutput = child_stdout_write;
    si.hStdError = child_stderr_write;

    PROCESS_INFORMATION pi{};
    std::wstring cmdline = build_windows_command_line(argv);
    std::vector<wchar_t> mutable_cmdline(cmdline.begin(), cmdline.end());
    mutable_cmdline.push_back(L'\0');

    std::wstring wide_cwd = cwd.empty() ? std::wstring{} : utf8_to_wide(cwd);
    BOOL ok = CreateProcessW(nullptr,
                             mutable_cmdline.data(),
                             nullptr,
                             nullptr,
                             TRUE,
                             CREATE_NO_WINDOW,
                             nullptr,
                             wide_cwd.empty() ? nullptr : wide_cwd.c_str(),
                             &si,
                             &pi);
    close_handle(child_stdin_read);
    close_handle(child_stdout_write);
    close_handle(child_stderr_write);
    if (!ok) {
        result.error = "CreateProcess failed: " + windows_error_message(GetLastError());
        cleanup();
        return finish();
    }
    result.started = true;
    CloseHandle(pi.hThread);

    if (!stdin_text.empty()) {
        const char* ptr = stdin_text.data();
        std::size_t remaining = stdin_text.size();
        while (remaining > 0) {
            DWORD written = 0;
            DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, 64 * 1024));
            if (!WriteFile(child_stdin_write, ptr, chunk, &written, nullptr)) break;
            if (written == 0) break;
            ptr += written;
            remaining -= written;
        }
    }
    close_handle(child_stdin_write);

    char buffer[4096];
    auto drain_pipe = [&](HANDLE pipe, std::string& out) {
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr)) break;
            if (avail == 0) break;
            DWORD bytes_read = 0;
            if (!ReadFile(pipe, buffer,
                          std::min<DWORD>(avail, static_cast<DWORD>(sizeof(buffer))),
                          &bytes_read, nullptr)) {
                break;
            }
            if (bytes_read == 0) break;
            append_capped(out, buffer, bytes_read);
        }
    };
    auto drain_output = [&]() {
        drain_pipe(child_stdout_read, result.stdout_text);
        drain_pipe(child_stderr_read, result.stderr_text);
    };

    const bool has_timeout = timeout_ms > 0;
    for (;;) {
        drain_output();
        DWORD wait = WaitForSingleObject(pi.hProcess, 0);
        if (wait == WAIT_OBJECT_0) break;
        if (has_timeout && elapsed_ms_since(started_at) >= timeout_ms) {
            result.timed_out = true;
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 1000);
            break;
        }
        Sleep(10);
    }
    drain_output();

    DWORD exit_code = 1;
    if (GetExitCodeProcess(pi.hProcess, &exit_code)) {
        result.exit_code = static_cast<int>(exit_code);
    }
    close_handle(pi.hProcess);
    close_handle(child_stdout_read);
    close_handle(child_stderr_read);
    return finish();
#else
    std::signal(SIGPIPE, SIG_IGN);

    int stdin_pipe[2] = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    if (pipe(stdin_pipe) != 0) {
        result.error = std::string("pipe(stdin) failed: ") + std::strerror(errno);
        return finish();
    }
    if (pipe(stdout_pipe) != 0) {
        result.error = std::string("pipe(stdout) failed: ") + std::strerror(errno);
        close_fd(stdin_pipe[0]);
        close_fd(stdin_pipe[1]);
        return finish();
    }
    if (pipe(stderr_pipe) != 0) {
        result.error = std::string("pipe(stderr) failed: ") + std::strerror(errno);
        close_fd(stdin_pipe[0]);
        close_fd(stdin_pipe[1]);
        close_fd(stdout_pipe[0]);
        close_fd(stdout_pipe[1]);
        return finish();
    }

    pid_t pid = fork();
    if (pid < 0) {
        result.error = std::string("fork failed: ") + std::strerror(errno);
        close_fd(stdin_pipe[0]);
        close_fd(stdin_pipe[1]);
        close_fd(stdout_pipe[0]);
        close_fd(stdout_pipe[1]);
        close_fd(stderr_pipe[0]);
        close_fd(stderr_pipe[1]);
        return finish();
    }

    if (pid == 0) {
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
        if (!cwd.empty()) {
            chdir(cwd.c_str());
        }

        std::vector<char*> cargv;
        cargv.reserve(argv.size() + 1);
        for (const auto& item : argv) {
            cargv.push_back(const_cast<char*>(item.c_str()));
        }
        cargv.push_back(nullptr);
        execvp(cargv[0], cargv.data());
        _exit(127);
    }

    result.started = true;
    close_fd(stdin_pipe[0]);
    close_fd(stdout_pipe[1]);
    close_fd(stderr_pipe[1]);
    int flags = fcntl(stdout_pipe[0], F_GETFL, 0);
    if (flags >= 0) fcntl(stdout_pipe[0], F_SETFL, flags | O_NONBLOCK);
    flags = fcntl(stderr_pipe[0], F_GETFL, 0);
    if (flags >= 0) fcntl(stderr_pipe[0], F_SETFL, flags | O_NONBLOCK);

    if (!stdin_text.empty()) {
        const char* ptr = stdin_text.data();
        std::size_t remaining = stdin_text.size();
        while (remaining > 0) {
            ssize_t written = write(stdin_pipe[1], ptr, remaining);
            if (written <= 0) break;
            ptr += written;
            remaining -= static_cast<std::size_t>(written);
        }
    }
    close_fd(stdin_pipe[1]);

    char buffer[4096];
    auto drain_fd = [&](int fd, std::string& out) {
        for (;;) {
            ssize_t n = read(fd, buffer, sizeof(buffer));
            if (n > 0) {
                append_capped(out, buffer, static_cast<std::size_t>(n));
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) break;
            break;
        }
    };
    auto drain_output = [&]() {
        drain_fd(stdout_pipe[0], result.stdout_text);
        drain_fd(stderr_pipe[0], result.stderr_text);
    };

    int status = 0;
    const bool has_timeout = timeout_ms > 0;
    for (;;) {
        drain_output();
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) break;
        if (done < 0 && errno != EINTR) break;
        if (has_timeout && elapsed_ms_since(started_at) >= timeout_ms) {
            result.timed_out = true;
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    drain_output();

    if (WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) result.exit_code = 128 + WTERMSIG(status);
    close_fd(stdout_pipe[0]);
    close_fd(stderr_pipe[0]);
    return finish();
#endif
}

HookProcessResult run_hook_shell_command(const std::string& command,
                                         const std::string& stdin_text,
                                         int timeout_ms,
                                         const std::string& cwd) {
    HookCommandSpec spec;
#ifdef _WIN32
    const char* comspec = std::getenv("COMSPEC");
    spec.command = (comspec && *comspec) ? std::string(comspec) : std::string("cmd.exe");
    spec.args = {"/d", "/s", "/c", command};
#else
    const char* shell = std::getenv("SHELL");
    spec.command = (shell && *shell) ? std::string(shell) : std::string("/bin/sh");
    spec.args = {"-c", command};
#endif
    return run_hook_process(spec, stdin_text, timeout_ms, cwd);
}

} // namespace acecode
