#include "lsp_process.hpp"

#include "../utils/logger.hpp"
#include "../utils/utf8_path.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>

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
#include <chrono>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#endif

namespace acecode::lsp {
namespace {

#ifdef _WIN32

std::string windows_error_message(DWORD code) {
    LPSTR buffer = nullptr;
    DWORD size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&buffer), 0, nullptr);
    std::string out = size > 0 && buffer ? std::string(buffer, size) : std::string{};
    if (buffer) LocalFree(buffer);
    while (!out.empty() && (out.back() == '\r' || out.back() == '\n')) out.pop_back();
    if (out.empty()) out = "Windows error " + std::to_string(code);
    return out;
}

void close_handle(void*& handle) {
    if (handle) {
        CloseHandle(static_cast<HANDLE>(handle));
        handle = nullptr;
    }
}

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool is_batch_script(const std::string& path) {
    const std::string lower = lower_ascii(path);
    return lower.size() > 4 &&
           (lower.compare(lower.size() - 4, 4, ".cmd") == 0 ||
            lower.compare(lower.size() - 4, 4, ".bat") == 0);
}

// 父环境 + 追加变量 → CreateProcessW 环境块(双 NUL 结尾的宽字符串序列)。
// Windows 环境变量名大小写不敏感,合并时按大写键去重。
std::wstring build_environment_block(
    const std::vector<std::pair<std::string, std::string>>& extra_env) {
    std::map<std::wstring, std::wstring> merged;
    LPWCH raw = GetEnvironmentStringsW();
    if (raw) {
        for (LPWCH cursor = raw; *cursor;) {
            std::wstring entry(cursor);
            cursor += entry.size() + 1;
            const std::size_t eq = entry.find(L'=');
            // 形如 "=C:=C:\\" 的驱动器项 key 以 '=' 开头,原样保留。
            if (eq == std::wstring::npos || eq == 0) {
                merged[entry] = std::wstring{};
                continue;
            }
            std::wstring key = entry.substr(0, eq);
            std::wstring upper = key;
            for (auto& ch : upper) ch = static_cast<wchar_t>(::towupper(ch));
            merged[upper] = entry.substr(eq + 1);
        }
        FreeEnvironmentStringsW(raw);
    }
    for (const auto& [key, value] : extra_env) {
        std::wstring wkey = utf8_to_wide(key);
        for (auto& ch : wkey) ch = static_cast<wchar_t>(::towupper(ch));
        merged[wkey] = utf8_to_wide(value);
    }

    std::wstring block;
    for (const auto& [key, value] : merged) {
        if (value.empty() && key.find(L'=') != std::wstring::npos) {
            block += key; // 驱动器项已含 '='
        } else {
            block += key + L"=" + value;
        }
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
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

std::string quote_windows_arg(const std::string& arg) {
    // 标准 MSVCRT 解析规则:无空白/引号且非空 → 原样;否则包引号,
    // 引号前的反斜杠序列翻倍,内部引号前插反斜杠。
    if (!arg.empty() &&
        arg.find_first_of(" \t\n\v\"") == std::string::npos) {
        return arg;
    }
    std::string out = "\"";
    std::size_t backslashes = 0;
    for (char ch : arg) {
        if (ch == '\\') {
            ++backslashes;
            continue;
        }
        if (ch == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out.push_back('"');
        } else {
            out.append(backslashes, '\\');
            out.push_back(ch);
        }
        backslashes = 0;
    }
    out.append(backslashes * 2, '\\');
    out.push_back('"');
    return out;
}

LspProcess::~LspProcess() {
    terminate();
}

bool LspProcess::start(const LspSpawnOptions& opts, std::string* error) {
    if (opts.argv.empty()) {
        if (error) *error = "empty argv";
        return false;
    }

#ifdef _WIN32
    std::vector<std::string> argv = opts.argv;
    if (is_batch_script(argv[0])) {
        // CreateProcess 不能直接执行 .cmd/.bat;npm 全局 shim 都是批处理。
        std::string comspec = "cmd.exe";
        std::vector<std::string> wrapped = {comspec, "/d", "/c"};
        wrapped.insert(wrapped.end(), argv.begin(), argv.end());
        argv = std::move(wrapped);
    }

    std::string command_line;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i) command_line.push_back(' ');
        command_line += quote_windows_arg(argv[i]);
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;

    HANDLE child_stdin_read = nullptr;
    HANDLE child_stdin_write = nullptr;
    HANDLE child_stdout_read = nullptr;
    HANDLE child_stdout_write = nullptr;
    HANDLE child_stderr = nullptr;

    auto cleanup = [&] {
        if (child_stdin_read) CloseHandle(child_stdin_read);
        if (child_stdin_write) CloseHandle(child_stdin_write);
        if (child_stdout_read) CloseHandle(child_stdout_read);
        if (child_stdout_write) CloseHandle(child_stdout_write);
        if (child_stderr) CloseHandle(child_stderr);
    };

    if (!CreatePipe(&child_stdin_read, &child_stdin_write, &sa, 0)) {
        if (error) *error = "CreatePipe(stdin) failed: " + windows_error_message(GetLastError());
        return false;
    }
    if (!SetHandleInformation(child_stdin_write, HANDLE_FLAG_INHERIT, 0)) {
        if (error) *error = "SetHandleInformation(stdin) failed: " +
            windows_error_message(GetLastError());
        cleanup();
        return false;
    }
    if (!CreatePipe(&child_stdout_read, &child_stdout_write, &sa, 0)) {
        if (error) *error = "CreatePipe(stdout) failed: " + windows_error_message(GetLastError());
        cleanup();
        return false;
    }
    if (!SetHandleInformation(child_stdout_read, HANDLE_FLAG_INHERIT, 0)) {
        if (error) *error = "SetHandleInformation(stdout) failed: " +
            windows_error_message(GetLastError());
        cleanup();
        return false;
    }

    // stderr 丢弃:LSP server 的日志会污染协议侧观察,统一送 NUL。
    child_stderr = CreateFileA("NUL", GENERIC_WRITE,
                               FILE_SHARE_WRITE | FILE_SHARE_READ, &sa,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (child_stderr == INVALID_HANDLE_VALUE) child_stderr = nullptr;

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = child_stdin_read;
    si.hStdOutput = child_stdout_write;
    si.hStdError = child_stderr ? child_stderr : child_stdout_write;

    PROCESS_INFORMATION pi{};
    std::wstring wide_command = utf8_to_wide(command_line);
    std::vector<wchar_t> mutable_command(wide_command.begin(), wide_command.end());
    mutable_command.push_back(L'\0');

    std::wstring env_block;
    LPVOID env_ptr = nullptr;
    DWORD creation_flags = CREATE_NO_WINDOW;
    if (!opts.extra_env.empty()) {
        env_block = build_environment_block(opts.extra_env);
        env_ptr = env_block.data();
        creation_flags |= CREATE_UNICODE_ENVIRONMENT;
    }

    std::wstring wide_cwd = opts.cwd.empty() ? std::wstring{} : utf8_to_wide(opts.cwd);

    BOOL ok = CreateProcessW(nullptr,
                             mutable_command.data(),
                             nullptr, nullptr,
                             TRUE,
                             creation_flags,
                             env_ptr,
                             wide_cwd.empty() ? nullptr : wide_cwd.c_str(),
                             &si, &pi);
    if (!ok) {
        if (error) *error = "CreateProcess failed for `" + command_line + "`: " +
            windows_error_message(GetLastError());
        cleanup();
        return false;
    }

    CloseHandle(child_stdin_read);
    CloseHandle(child_stdout_write);
    if (child_stderr) CloseHandle(child_stderr);
    CloseHandle(pi.hThread);

    process_handle_ = pi.hProcess;
    stdin_write_ = child_stdin_write;
    stdout_read_ = child_stdout_read;
    started_ = true;
    return true;
#else
    int stdin_pipe[2] = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    if (pipe(stdin_pipe) != 0) {
        if (error) *error = std::string("pipe(stdin) failed: ") + std::strerror(errno);
        return false;
    }
    if (pipe(stdout_pipe) != 0) {
        if (error) *error = std::string("pipe(stdout) failed: ") + std::strerror(errno);
        close_fd(stdin_pipe[0]);
        close_fd(stdin_pipe[1]);
        return false;
    }

    std::vector<std::string> argv = opts.argv;
    pid_t pid = fork();
    if (pid < 0) {
        if (error) *error = std::string("fork failed: ") + std::strerror(errno);
        close_fd(stdin_pipe[0]);
        close_fd(stdin_pipe[1]);
        close_fd(stdout_pipe[0]);
        close_fd(stdout_pipe[1]);
        return false;
    }

    if (pid == 0) {
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        int dev_null = open("/dev/null", O_WRONLY);
        if (dev_null >= 0) {
            dup2(dev_null, STDERR_FILENO);
            close(dev_null);
        }
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        if (!opts.cwd.empty()) {
            if (chdir(opts.cwd.c_str()) != 0) _exit(126);
        }
        for (const auto& [key, value] : opts.extra_env) {
            setenv(key.c_str(), value.c_str(), 1);
        }
        std::vector<char*> exec_argv;
        exec_argv.reserve(argv.size() + 1);
        for (auto& arg : argv) exec_argv.push_back(arg.data());
        exec_argv.push_back(nullptr);
        execvp(exec_argv[0], exec_argv.data());
        _exit(127);
    }

    close_fd(stdin_pipe[0]);
    close_fd(stdout_pipe[1]);
    process_id_ = pid;
    stdin_write_ = stdin_pipe[1];
    stdout_read_ = stdout_pipe[0];
    started_ = true;
    return true;
#endif
}

long LspProcess::read_stdout(char* buf, std::size_t len) {
#ifdef _WIN32
    if (!stdout_read_) return 0;
    DWORD bytes_read = 0;
    BOOL ok = ReadFile(static_cast<HANDLE>(stdout_read_), buf,
                       static_cast<DWORD>(len), &bytes_read, nullptr);
    if (!ok) {
        const DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_HANDLE_EOF) return 0;
        return -1;
    }
    return static_cast<long>(bytes_read);
#else
    if (stdout_read_ < 0) return 0;
    for (;;) {
        ssize_t n = read(stdout_read_, buf, len);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return -1;
        return static_cast<long>(n);
    }
#endif
}

bool LspProcess::write_stdin(const char* data, std::size_t len, std::string* error) {
#ifdef _WIN32
    if (!stdin_write_) {
        if (error) *error = "stdin closed";
        return false;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(static_cast<HANDLE>(stdin_write_), data,
                        static_cast<DWORD>(len), &written, nullptr);
    if (!ok || written != len) {
        if (error) *error = "WriteFile(stdin) failed: " +
            windows_error_message(GetLastError());
        return false;
    }
    return true;
#else
    if (stdin_write_ < 0) {
        if (error) *error = "stdin closed";
        return false;
    }
    const char* cursor = data;
    std::size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = write(stdin_write_, cursor, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (error) *error = std::string("write(stdin) failed: ") + std::strerror(errno);
            return false;
        }
        cursor += n;
        remaining -= static_cast<std::size_t>(n);
    }
    return true;
#endif
}

void LspProcess::close_stdin() {
#ifdef _WIN32
    close_handle(stdin_write_);
#else
    close_fd(stdin_write_);
#endif
}

bool LspProcess::wait_exit(int timeout_ms) {
#ifdef _WIN32
    if (!process_handle_) return true;
    return WaitForSingleObject(static_cast<HANDLE>(process_handle_),
                               static_cast<DWORD>(timeout_ms)) == WAIT_OBJECT_0;
#else
    if (process_id_ <= 0) return true;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    for (;;) {
        int status = 0;
        pid_t r = waitpid(process_id_, &status, WNOHANG);
        if (r == process_id_ || (r < 0 && errno == ECHILD)) {
            process_id_ = -1;
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
#endif
}

void LspProcess::terminate() {
#ifdef _WIN32
    if (process_handle_) {
        TerminateProcess(static_cast<HANDLE>(process_handle_), 0);
    }
    close_handle(stdin_write_);
    close_handle(stdout_read_);
    if (process_handle_) {
        WaitForSingleObject(static_cast<HANDLE>(process_handle_), 2000);
        close_handle(process_handle_);
    }
#else
    if (process_id_ > 0) {
        kill(process_id_, SIGKILL);
    }
    close_fd(stdin_write_);
    close_fd(stdout_read_);
    if (process_id_ > 0) {
        wait_exit(2000);
        process_id_ = -1;
    }
#endif
    started_ = false;
}

} // namespace acecode::lsp
