#include "client.hpp"

#include "utils/encoding.hpp"
#include "utils/logger.hpp"
#include "utils/utf8_path.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits.h>
#include <mutex>
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
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

namespace acecode::ace_browser_bridge {
namespace {

constexpr const char* kDefaultHostPort = "52007";

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

std::string default_host_executable_name() {
#ifdef _WIN32
    return "ace-browser-host.exe";
#else
    return "ace-browser-host";
#endif
}

std::filesystem::path current_executable_dir() {
#ifdef _WIN32
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        DWORD n = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (n == 0) return {};
        if (n < buffer.size()) {
            return std::filesystem::path(std::wstring(buffer.data(), n)).parent_path();
        }
        if (buffer.size() >= 32768) return {};
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    if (size == 0) return {};
    std::vector<char> buffer(size + 1);
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    return std::filesystem::path(buffer.data()).parent_path();
#elif defined(__linux__)
    char buffer[PATH_MAX];
    ssize_t n = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (n <= 0) return {};
    buffer[n] = '\0';
    return std::filesystem::path(buffer).parent_path();
#else
    return {};
#endif
}

std::string resolve_host_path(const AceBrowserBridgeConfig& config) {
    if (!config.host_path.empty()) return config.host_path;
    const auto dir = current_executable_dir();
    if (!dir.empty()) {
        return path_to_utf8(dir / default_host_executable_name());
    }
    return default_host_executable_name();
}

bool error_code_is(const BridgeEnvelope& envelope, const char* code) {
    return !envelope.ok && envelope.error && envelope.error->code == code;
}

std::string bool_text(bool value) {
    return value ? "true" : "false";
}

long long elapsed_ms_since(std::chrono::steady_clock::time_point started) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
}

std::string cli_args_for_log(const std::vector<std::string>& args) {
    std::ostringstream oss;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i > 0) oss << ' ';
        oss << args[i];
    }
    return acecode::log_truncate(oss.str(), 300);
}

std::string envelope_summary(const BridgeEnvelope& envelope) {
    std::ostringstream oss;
    oss << "ok=" << bool_text(envelope.ok);
    if (envelope.error) oss << " error_code=" << envelope.error->code;
    if (envelope.data.is_object()) {
        if (envelope.data.contains("running")) {
            oss << " running=" << bool_text(envelope.data.value("running", false));
        }
        if (envelope.data.contains("ready")) {
            oss << " ready=" << bool_text(envelope.data.value("ready", false));
        }
        if (envelope.data.contains("extension_connected")) {
            oss << " extension_connected=" << bool_text(envelope.data.value("extension_connected", false));
        }
        if (envelope.data.contains("extension_stale")) {
            oss << " extension_stale=" << bool_text(envelope.data.value("extension_stale", false));
        }
        if (envelope.data.contains("queued_actions")) {
            oss << " queued_actions=" << envelope.data.value("queued_actions", 0);
        }
        if (envelope.data.contains("pending_actions")) {
            oss << " pending_actions=" << envelope.data.value("pending_actions", 0);
        }
    }
    return oss.str();
}

std::tm local_tm(std::time_t time) {
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &time);
#else
    localtime_r(&time, &tm_buf);
#endif
    return tm_buf;
}

std::string date_string(const std::tm& tm_buf) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                  tm_buf.tm_year + 1900,
                  tm_buf.tm_mon + 1,
                  tm_buf.tm_mday);
    return std::string(buf);
}

std::mutex& browser_agent_log_mutex() {
    static std::mutex mu;
    return mu;
}

void browser_agent_file_log(const char* level, const std::string& message) {
    std::lock_guard<std::mutex> lk(browser_agent_log_mutex());
    try {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;
        std::tm tm_buf = local_tm(time);

        std::error_code ec;
        const auto dir = acecode::path_from_utf8(acecode::get_logs_dir());
        std::filesystem::create_directories(dir, ec);
        if (ec) return;

        std::ofstream out(dir / ("ace-browser-agent-" + date_string(tm_buf) + ".log"),
                          std::ios::out | std::ios::app);
        if (!out.is_open()) return;
        out << std::put_time(&tm_buf, "%H:%M:%S") << "."
            << std::setfill('0') << std::setw(3) << ms.count()
            << " " << level << " " << message << "\n";
    } catch (...) {
        // Browser diagnostics must not affect tool execution.
    }
}

void browser_agent_info(const std::string& message) {
    LOG_INFO(message);
    browser_agent_file_log("INF", message);
}

void browser_agent_warn(const std::string& message) {
    LOG_WARN(message);
    browser_agent_file_log("WRN", message);
}

} // namespace

AceBrowserBridgeClient::AceBrowserBridgeClient(AceBrowserBridgeConfig config,
                                               CliRunner runner,
                                               HostStarter host_starter)
    : config_(std::move(config)) {
    const bool custom_runner = static_cast<bool>(runner);
    runner_ = custom_runner ? std::move(runner) : CliRunner(run_cli_process);
    host_starter_ = host_starter ? std::move(host_starter)
                                 : (custom_runner ? HostStarter{} : HostStarter(start_host_process));
}

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
        return make_error("invalid_host_response", "ace-browser-host returned an invalid JSON envelope");
    }

    BridgeEnvelope envelope;
    envelope.ok = parsed["ok"].get<bool>();
    if (envelope.ok) {
        envelope.data = parsed.contains("data") ? parsed["data"] : nlohmann::json::object();
        if (envelope.data.is_null()) envelope.data = nlohmann::json::object();
        return envelope;
    }

    if (!parsed.contains("error") || !parsed["error"].is_object()) {
        return make_error("invalid_host_response", "ace-browser-host returned an invalid error envelope");
    }
    const auto& err = parsed["error"];
    if (!err.contains("code") || !err["code"].is_string() ||
        !err.contains("message") || !err["message"].is_string()) {
        return make_error("invalid_host_response", "ace-browser-host error envelope is missing code or message");
    }
    envelope.error = BridgeError{err["code"].get<std::string>(),
                                 err["message"].get<std::string>()};
    return envelope;
}

BridgeEnvelope AceBrowserBridgeClient::run_json_command(const std::vector<std::string>& args,
                                                        const std::string& stdin_text) {
    std::vector<std::string> argv;
    argv.reserve(args.size() + 1);
    argv.push_back(resolve_host_path(config_));
    argv.insert(argv.end(), args.begin(), args.end());

    const auto started = std::chrono::steady_clock::now();
    browser_agent_info("[ace-browser-client] cli start args=" + cli_args_for_log(args) +
                       " stdin_bytes=" + std::to_string(stdin_text.size()));
    CliProcessResult result = runner_(argv, stdin_text, std::chrono::milliseconds(config_.tool_timeout_ms));
    if (result.timed_out) {
        browser_agent_warn("[ace-browser-client] cli timeout args=" + cli_args_for_log(args) +
                           " duration_ms=" + std::to_string(elapsed_ms_since(started)));
        return make_error("host_timeout", "ace-browser-host timed out");
    }
    if (!result.error.empty()) {
        browser_agent_warn("[ace-browser-client] cli error args=" + cli_args_for_log(args) +
                           " error=" + acecode::log_truncate(result.error) +
                           " duration_ms=" + std::to_string(elapsed_ms_since(started)));
        return make_error("host_not_found", result.error);
    }
    if (result.exit_code != 0 && result.stdout_text.empty()) {
        browser_agent_warn("[ace-browser-client] cli failed args=" + cli_args_for_log(args) +
                           " exit_code=" + std::to_string(result.exit_code) +
                           " duration_ms=" + std::to_string(elapsed_ms_since(started)));
        return make_error("host_failed", "ace-browser-host exited with code " +
                                        std::to_string(result.exit_code));
    }
    BridgeEnvelope envelope = parse_envelope(result.stdout_text);
    const std::string line = "[ace-browser-client] cli finish args=" + cli_args_for_log(args) +
        " " + envelope_summary(envelope) +
        " exit_code=" + std::to_string(result.exit_code) +
        " stdout_bytes=" + std::to_string(result.stdout_text.size()) +
        " duration_ms=" + std::to_string(elapsed_ms_since(started));
    if (envelope.ok) {
        browser_agent_info(line);
    } else {
        browser_agent_warn(line);
    }
    return envelope;
}

bool AceBrowserBridgeClient::should_cache_status(const BridgeEnvelope& envelope) const {
    if (!envelope.ok || !envelope.data.is_object()) return false;
    return envelope.data.value("running", false) &&
           envelope.data.value("extension_connected", false);
}

BridgeEnvelope AceBrowserBridgeClient::status_once() {
    return run_json_command({"status", "--json"}, "");
}

HostStartResult AceBrowserBridgeClient::start_host_daemon() {
    if (!host_starter_) {
        browser_agent_warn("[ace-browser-client] host autostart unavailable");
        return HostStartResult{false, "ace-browser-host auto-start is not available"};
    }
    std::vector<std::string> argv{
        resolve_host_path(config_),
        "serve",
        "--json",
        "--port",
        kDefaultHostPort,
    };
    browser_agent_info("[ace-browser-client] host autostart start args=" + cli_args_for_log(argv));
    HostStartResult result = host_starter_(argv);
    if (result.ok) {
        browser_agent_info("[ace-browser-client] host autostart launched port=" + std::string(kDefaultHostPort));
    } else {
        browser_agent_warn("[ace-browser-client] host autostart failed error=" +
                           acecode::log_truncate(result.error));
    }
    return result;
}

BridgeEnvelope AceBrowserBridgeClient::ensure_host_running_from_status(BridgeEnvelope status_envelope) {
    if (!status_envelope.ok || !status_envelope.data.is_object() ||
        status_envelope.data.value("running", false)) {
        return status_envelope;
    }

    HostStartResult start = start_host_daemon();
    status_envelope.data["auto_start_attempted"] = true;
    if (!start.ok) {
        status_envelope.data["auto_start_error"] =
            start.error.empty() ? "ace-browser-host failed to start" : start.error;
        return status_envelope;
    }

    clear_status_cache();
    BridgeEnvelope latest = status_envelope;
    for (int attempt = 0; attempt < 20; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(attempt < 5 ? 100 : 250));
        latest = status_once();
        if (!latest.ok) return latest;
        if (latest.data.is_object()) {
            latest.data["auto_start_attempted"] = true;
            latest.data["auto_started"] = true;
        }
        if (latest.data.value("running", false)) return latest;
    }

    if (latest.ok && latest.data.is_object()) {
        latest.data["auto_start_attempted"] = true;
        latest.data["auto_started"] = true;
        latest.data["auto_start_error"] = "ace-browser-host did not report running after start";
    }
    return latest;
}

BridgeEnvelope AceBrowserBridgeClient::status() {
    const auto now = std::chrono::steady_clock::now();
    if (cached_status_.has_value()) {
        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - cached_status_at_);
        if (age.count() <= config_.status_cache_ttl_ms) {
            return *cached_status_;
        }
    }

    BridgeEnvelope envelope = ensure_host_running_from_status(status_once());
    if (should_cache_status(envelope)) {
        cached_status_ = envelope;
        cached_status_at_ = now;
    } else {
        cached_status_.reset();
    }
    browser_agent_info("[ace-browser-client] status refresh " + envelope_summary(envelope));
    return envelope;
}

BridgeEnvelope AceBrowserBridgeClient::ensure_ready() {
    clear_status_cache();
    const auto started = std::chrono::steady_clock::now();
    browser_agent_info("[ace-browser-client] ensure-ready start");
    BridgeEnvelope envelope = run_json_command({"ensure-ready", "--json"}, "");
    if (should_cache_status(envelope)) {
        cached_status_ = envelope;
        cached_status_at_ = std::chrono::steady_clock::now();
    } else {
        cached_status_.reset();
    }
    const std::string line = "[ace-browser-client] ensure-ready finish " +
        envelope_summary(envelope) +
        " duration_ms=" + std::to_string(elapsed_ms_since(started));
    if (envelope.ok) {
        browser_agent_info(line);
    } else {
        browser_agent_warn(line);
    }
    return envelope;
}

BridgeEnvelope AceBrowserBridgeClient::command(const BrowserCommandRequest& request) {
    const std::string body = command_request_json(request).dump();
    const auto started = std::chrono::steady_clock::now();
    browser_agent_info("[ace-browser-client] command start action=" + request.action +
                       " session=" + request.session);
    BridgeEnvelope envelope = run_json_command({"command", "--json"}, body);
    if (!error_code_is(envelope, "daemon_not_running")) {
        const std::string line = "[ace-browser-client] command finish action=" + request.action +
            " session=" + request.session +
            " " + envelope_summary(envelope) +
            " duration_ms=" + std::to_string(elapsed_ms_since(started));
        envelope.ok ? browser_agent_info(line) : browser_agent_warn(line);
        return envelope;
    }

    browser_agent_warn("[ace-browser-client] command daemon_not_running action=" + request.action +
                       " session=" + request.session +
                       " attempting_autostart=true");
    BridgeEnvelope status_envelope = ensure_host_running_from_status(status_once());
    if (!status_envelope.ok) return status_envelope;
    if (!status_envelope.data.value("running", false)) return envelope;
    envelope = run_json_command({"command", "--json"}, body);
    const std::string line = "[ace-browser-client] command finish action=" + request.action +
        " session=" + request.session +
        " " + envelope_summary(envelope) +
        " retried_after_autostart=true" +
        " duration_ms=" + std::to_string(elapsed_ms_since(started));
    envelope.ok ? browser_agent_info(line) : browser_agent_warn(line);
    return envelope;
}

BridgeEnvelope AceBrowserBridgeClient::screenshot(const std::string& session,
                                                  const std::string& output_path) {
    std::vector<std::string> args = {"screenshot", "--json", "--session", session, "--output", output_path};
    const auto started = std::chrono::steady_clock::now();
    browser_agent_info("[ace-browser-client] screenshot start session=" + session +
                       " output=" + acecode::log_truncate(output_path, 300));
    BridgeEnvelope envelope = run_json_command(args, "");
    if (!error_code_is(envelope, "daemon_not_running")) {
        const std::string line = "[ace-browser-client] screenshot finish session=" + session +
            " " + envelope_summary(envelope) +
            " duration_ms=" + std::to_string(elapsed_ms_since(started));
        envelope.ok ? browser_agent_info(line) : browser_agent_warn(line);
        return envelope;
    }

    browser_agent_warn("[ace-browser-client] screenshot daemon_not_running session=" + session +
                       " attempting_autostart=true");
    BridgeEnvelope status_envelope = ensure_host_running_from_status(status_once());
    if (!status_envelope.ok) return status_envelope;
    if (!status_envelope.data.value("running", false)) return envelope;
    envelope = run_json_command(args, "");
    const std::string line = "[ace-browser-client] screenshot finish session=" + session +
        " " + envelope_summary(envelope) +
        " retried_after_autostart=true" +
        " duration_ms=" + std::to_string(elapsed_ms_since(started));
    envelope.ok ? browser_agent_info(line) : browser_agent_warn(line);
    return envelope;
}

void AceBrowserBridgeClient::clear_status_cache() {
    cached_status_.reset();
}

CliProcessResult run_cli_process(const std::vector<std::string>& argv,
                                 const std::string& stdin_text,
                                 std::chrono::milliseconds timeout) {
    CliProcessResult result;
    if (argv.empty() || argv[0].empty()) {
        result.error = "ace-browser-host path is empty";
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
        result.error = "Failed to start ace-browser-host: " + windows_error_message(GetLastError());
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

HostStartResult start_host_process(const std::vector<std::string>& argv) {
    if (argv.empty() || argv[0].empty()) {
        return HostStartResult{false, "ace-browser-host path is empty"};
    }

#ifdef _WIN32
    STARTUPINFOW si{};
    si.cb = sizeof(si);

    PROCESS_INFORMATION pi{};
    std::wstring command_line = build_windows_command_line(argv);
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');

    std::wstring cwd;
    try {
        cwd = std::filesystem::path(utf8_to_wide(argv[0])).parent_path().wstring();
    } catch (...) {
        cwd.clear();
    }

    BOOL ok = CreateProcessW(nullptr,
                             mutable_command.data(),
                             nullptr,
                             nullptr,
                             FALSE,
                             CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
                             nullptr,
                             cwd.empty() ? nullptr : cwd.c_str(),
                             &si,
                             &pi);
    if (!ok) {
        return HostStartResult{false, "Failed to start ace-browser-host: " +
                                          windows_error_message(GetLastError())};
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return HostStartResult{true, ""};
#else
    pid_t pid = fork();
    if (pid < 0) {
        return HostStartResult{false, std::string("fork failed: ") + std::strerror(errno)};
    }
    if (pid == 0) {
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
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
    return HostStartResult{true, ""};
#endif
}

} // namespace acecode::ace_browser_bridge
