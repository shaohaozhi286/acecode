#include "client.hpp"

#include "utils/encoding.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace acecode::ace_browser_bridge {
namespace {

BridgeEnvelope make_error(std::string code, std::string message) {
    BridgeEnvelope envelope;
    envelope.ok = false;
    envelope.error = BridgeError{std::move(code), std::move(message)};
    return envelope;
}

#ifdef _WIN32
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
    if (arg.empty()) return L"\"\"";
    bool needs_quotes = false;
    for (wchar_t ch : arg) {
        if (ch == L' ' || ch == L'\t' || ch == L'"') {
            needs_quotes = true;
            break;
        }
    }
    if (!needs_quotes) return arg;

    std::wstring out = L"\"";
    int backslashes = 0;
    for (wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
        } else if (ch == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(ch);
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
    std::wstring command;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i > 0) command.push_back(L' ');
        command += quote_windows_arg(argv[i]);
    }
    return command;
}

void close_handle(HANDLE& handle) {
    if (handle && handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
    }
    handle = nullptr;
}
#else
void close_fd(int& fd) {
    if (fd >= 0) close(fd);
    fd = -1;
}
#endif

} // namespace

AceBrowserBridgeClient::AceBrowserBridgeClient(AceBrowserBridgeConfig config,
                                               CliRunner runner)
    : config_(std::move(config)),
      runner_(runner ? std::move(runner) : run_cli_process) {}

nlohmann::json AceBrowserBridgeClient::command_request_json(const BrowserCommandRequest& request) {
    nlohmann::json j = nlohmann::json::object();
    j["session"] = request.session;
    j["action"] = request.action;
    j["args"] = request.args.is_null() ? nlohmann::json::object() : request.args;
    return j;
}

BridgeEnvelope AceBrowserBridgeClient::parse_envelope(const std::string& stdout_text) {
    auto parsed = nlohmann::json::parse(stdout_text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object() ||
        !parsed.contains("ok") || !parsed["ok"].is_boolean()) {
        return make_error("invalid_cli_response", "ace-browser-cli returned an invalid JSON envelope");
    }

    BridgeEnvelope envelope;
    envelope.ok = parsed["ok"].get<bool>();
    if (envelope.ok) {
        envelope.data = parsed.contains("data") ? parsed["data"] : nlohmann::json::object();
        if (envelope.data.is_null()) envelope.data = nlohmann::json::object();
        return envelope;
    }

    if (!parsed.contains("error") || !parsed["error"].is_object()) {
        return make_error("invalid_cli_response", "ace-browser-cli returned an invalid error envelope");
    }
    const auto& err = parsed["error"];
    if (!err.contains("code") || !err["code"].is_string() ||
        !err.contains("message") || !err["message"].is_string()) {
        return make_error("invalid_cli_response", "ace-browser-cli error envelope is missing code or message");
    }
    envelope.error = BridgeError{err["code"].get<std::string>(),
                                 err["message"].get<std::string>()};
    return envelope;
}

BridgeEnvelope AceBrowserBridgeClient::run_json_command(const std::vector<std::string>& args,
                                                        const std::string& stdin_text) {
    std::vector<std::string> argv;
    argv.reserve(args.size() + 1);
    argv.push_back(config_.cli_path);
    argv.insert(argv.end(), args.begin(), args.end());

    CliProcessResult result = runner_(argv, stdin_text, std::chrono::milliseconds(config_.tool_timeout_ms));
    if (result.timed_out) {
        return make_error("cli_timeout", "ace-browser-cli timed out");
    }
    if (!result.error.empty()) {
        return make_error("cli_not_found", result.error);
    }
    if (result.exit_code != 0 && result.stdout_text.empty()) {
        return make_error("cli_failed", "ace-browser-cli exited with code " +
                                        std::to_string(result.exit_code));
    }
    return parse_envelope(result.stdout_text);
}

bool AceBrowserBridgeClient::should_cache_status(const BridgeEnvelope& envelope) const {
    if (!envelope.ok || !envelope.data.is_object()) return false;
    return envelope.data.value("running", false) &&
           envelope.data.value("extension_connected", false);
}

BridgeEnvelope AceBrowserBridgeClient::status() {
    const auto now = std::chrono::steady_clock::now();
    if (cached_status_.has_value()) {
        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - cached_status_at_);
        if (age.count() <= config_.status_cache_ttl_ms) {
            return *cached_status_;
        }
    }

    BridgeEnvelope envelope = run_json_command({"status", "--json"}, "");
    if (should_cache_status(envelope)) {
        cached_status_ = envelope;
        cached_status_at_ = now;
    } else {
        cached_status_.reset();
    }
    return envelope;
}

BridgeEnvelope AceBrowserBridgeClient::command(const BrowserCommandRequest& request) {
    return run_json_command({"command", "--json"}, command_request_json(request).dump());
}

BridgeEnvelope AceBrowserBridgeClient::screenshot(const std::string& session,
                                                  const std::string& output_path) {
    return run_json_command({"screenshot", "--json", "--session", session, "--output", output_path}, "");
}

void AceBrowserBridgeClient::clear_status_cache() {
    cached_status_.reset();
}

CliProcessResult run_cli_process(const std::vector<std::string>& argv,
                                 const std::string& stdin_text,
                                 std::chrono::milliseconds timeout) {
    CliProcessResult result;
    if (argv.empty() || argv[0].empty()) {
        result.error = "ace-browser-cli path is empty";
        return result;
    }
    if (timeout.count() <= 0) timeout = std::chrono::milliseconds(30000);

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE child_stdin_read = nullptr;
    HANDLE child_stdin_write = nullptr;
    HANDLE child_stdout_read = nullptr;
    HANDLE child_stdout_write = nullptr;

    auto cleanup = [&]() {
        close_handle(child_stdin_read);
        close_handle(child_stdin_write);
        close_handle(child_stdout_read);
        close_handle(child_stdout_write);
    };

    if (!CreatePipe(&child_stdin_read, &child_stdin_write, &sa, 0)) {
        result.error = "CreatePipe(stdin) failed: " + windows_error_message(GetLastError());
        return result;
    }
    if (!SetHandleInformation(child_stdin_write, HANDLE_FLAG_INHERIT, 0)) {
        result.error = "SetHandleInformation(stdin) failed: " + windows_error_message(GetLastError());
        cleanup();
        return result;
    }
    if (!CreatePipe(&child_stdout_read, &child_stdout_write, &sa, 0)) {
        result.error = "CreatePipe(stdout) failed: " + windows_error_message(GetLastError());
        cleanup();
        return result;
    }
    if (!SetHandleInformation(child_stdout_read, HANDLE_FLAG_INHERIT, 0)) {
        result.error = "SetHandleInformation(stdout) failed: " + windows_error_message(GetLastError());
        cleanup();
        return result;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = child_stdin_read;
    si.hStdOutput = child_stdout_write;
    si.hStdError = child_stdout_write;

    PROCESS_INFORMATION pi{};
    std::wstring command_line = build_windows_command_line(argv);
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');

    BOOL ok = CreateProcessW(nullptr,
                             mutable_command.data(),
                             nullptr,
                             nullptr,
                             TRUE,
                             CREATE_NO_WINDOW,
                             nullptr,
                             nullptr,
                             &si,
                             &pi);
    close_handle(child_stdin_read);
    close_handle(child_stdout_write);
    if (!ok) {
        result.error = "Failed to start ace-browser-cli: " + windows_error_message(GetLastError());
        cleanup();
        return result;
    }
    CloseHandle(pi.hThread);

    std::thread reader([&]() {
        char buffer[4096];
        for (;;) {
            DWORD read = 0;
            BOOL read_ok = ReadFile(child_stdout_read, buffer, sizeof(buffer), &read, nullptr);
            if (!read_ok || read == 0) break;
            result.stdout_text.append(buffer, buffer + read);
        }
    });

    if (!stdin_text.empty()) {
        const char* ptr = stdin_text.data();
        std::size_t remaining = stdin_text.size();
        while (remaining > 0) {
            DWORD written = 0;
            DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(remaining, 64 * 1024));
            if (!WriteFile(child_stdin_write, ptr, chunk, &written, nullptr)) break;
            ptr += written;
            remaining -= written;
        }
    }
    close_handle(child_stdin_write);

    DWORD wait = WaitForSingleObject(pi.hProcess, static_cast<DWORD>(timeout.count()));
    if (wait == WAIT_TIMEOUT) {
        result.timed_out = true;
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 1000);
    }
    DWORD exit_code = 1;
    if (GetExitCodeProcess(pi.hProcess, &exit_code)) {
        result.exit_code = static_cast<int>(exit_code);
    }
    close_handle(pi.hProcess);
    if (reader.joinable()) reader.join();
    close_handle(child_stdout_read);
    return result;
#else
    std::signal(SIGPIPE, SIG_IGN);
    int stdin_pipe[2] = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    if (pipe(stdin_pipe) != 0) {
        result.error = std::string("pipe(stdin) failed: ") + std::strerror(errno);
        return result;
    }
    if (pipe(stdout_pipe) != 0) {
        result.error = std::string("pipe(stdout) failed: ") + std::strerror(errno);
        close_fd(stdin_pipe[0]);
        close_fd(stdin_pipe[1]);
        return result;
    }

    pid_t pid = fork();
    if (pid < 0) {
        result.error = std::string("fork failed: ") + std::strerror(errno);
        close_fd(stdin_pipe[0]);
        close_fd(stdin_pipe[1]);
        close_fd(stdout_pipe[0]);
        close_fd(stdout_pipe[1]);
        return result;
    }
    if (pid == 0) {
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stdout_pipe[1], STDERR_FILENO);
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);

        std::vector<char*> cargv;
        cargv.reserve(argv.size() + 1);
        for (const auto& item : argv) {
            cargv.push_back(const_cast<char*>(item.c_str()));
        }
        cargv.push_back(nullptr);
        execvp(cargv[0], cargv.data());
        _exit(127);
    }

    close_fd(stdin_pipe[0]);
    close_fd(stdout_pipe[1]);
    std::thread reader([&]() {
        char buffer[4096];
        for (;;) {
            ssize_t n = read(stdout_pipe[0], buffer, sizeof(buffer));
            if (n <= 0) break;
            result.stdout_text.append(buffer, static_cast<std::size_t>(n));
        }
    });

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

    int status = 0;
    auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) break;
        if (done < 0 && errno != EINTR) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            result.timed_out = true;
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (WIFEXITED(status)) result.exit_code = WEXITSTATUS(status);
    if (reader.joinable()) reader.join();
    close_fd(stdout_pipe[0]);
    return result;
#endif
}

} // namespace acecode::ace_browser_bridge
