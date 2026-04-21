#include "bash_tool.hpp"
#include "tool_icons.hpp"
#include "utils/logger.hpp"
#include "utils/encoding.hpp"
#include "utils/stream_processing.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <deque>
#include <chrono>
#include <filesystem>
#include <thread>
#include <atomic>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#endif

namespace acecode {

static constexpr int DEFAULT_TIMEOUT_MS = 120000;    // 2 minutes
static constexpr size_t MAX_OUTPUT_SIZE = 100 * 1024; // 100KB

// Normalize Windows CRLF to LF before feeding the line state machine, so the
// \r does not clobber `current_line` mid-way through a proper newline.
#ifdef _WIN32
static std::string normalize_crlf(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n') {
            out += '\n';
            i++;
        } else {
            out += s[i];
        }
    }
    return out;
}
#endif

static ToolResult execute_bash(const std::string& arguments_json, const ToolContext& ctx) {
    std::string command;
    int timeout_ms = DEFAULT_TIMEOUT_MS;
    std::string cwd;
    std::vector<std::string> stdin_inputs;

    try {
        auto args = nlohmann::json::parse(arguments_json);
        command = args.value("command", "");
        timeout_ms = args.value("timeout_ms", DEFAULT_TIMEOUT_MS);
        cwd = args.value("cwd", "");
        if (args.contains("stdin_inputs") && args["stdin_inputs"].is_array()) {
            for (const auto& el : args["stdin_inputs"]) {
                if (el.is_string()) stdin_inputs.push_back(el.get<std::string>());
            }
        }
    } catch (...) {
        return ToolResult{"[Error] Failed to parse tool arguments.", false};
    }

    if (command.empty()) {
        return ToolResult{"[Error] No command provided.", false};
    }

    auto t_start = std::chrono::steady_clock::now();
    auto make_summary = [&](const std::string& cmd, long long duration_ms,
                            size_t total_bytes_out, int exit_code,
                            bool is_success, bool was_truncated,
                            bool was_aborted, bool was_timed_out) {
        ToolSummary s;
        s.verb = "Ran";
        std::string preview = cmd;
        if (preview.size() > 60) preview = preview.substr(0, 57) + "...";
        s.object = preview;
        s.metrics.emplace_back("time", format_duration_compact(duration_ms));
        s.metrics.emplace_back("bytes", format_bytes_compact(total_bytes_out));
        if (!is_success && exit_code != 0) {
            s.metrics.emplace_back("exit", std::to_string(exit_code));
        }
        if (was_truncated) s.metrics.emplace_back("truncated", "true");
        if (was_aborted) s.metrics.emplace_back("aborted", "true");
        if (was_timed_out) s.metrics.emplace_back("timeout", "true");
        s.icon = tool_icon("bash");
        return s;
    };

    LOG_INFO("bash: cmd=" + log_truncate(command, 200) + " cwd=" + cwd +
             " timeout=" + std::to_string(timeout_ms) +
             " stdin_inputs=" + std::to_string(stdin_inputs.size()));

    if (!cwd.empty() && !std::filesystem::is_directory(cwd)) {
        ToolResult r{"[Error] Working directory does not exist: " + cwd, false};
        r.summary = make_summary(command, 0, 0, -1, false, false, false, false);
        return r;
    }

    // Shared streaming state across both OS branches.
    std::string full_output;
    std::string current_line;
    std::string utf8_pending;
    std::deque<std::string> tail_lines;
    int total_lines = 0;

    auto process_raw = [&](const char* data, size_t len) {
        if (len == 0) return;
        utf8_pending.append(data, len);
        size_t safe_end = utf8_safe_boundary(utf8_pending);
        std::string safe_part = utf8_pending.substr(0, safe_end);
        utf8_pending.erase(0, safe_end);
#ifdef _WIN32
        safe_part = normalize_crlf(safe_part);
#endif
        std::string clean = strip_ansi(safe_part);
        full_output += clean;
        feed_line_state(clean, current_line, tail_lines, total_lines);
        if (ctx.stream && !clean.empty()) ctx.stream(clean);
    };

    bool aborted = false;
    bool timed_out = false;

#ifdef _WIN32
    // stdin_inputs not implemented on Windows in this release; it's accepted
    // but silently ignored (see proposal — deferred to future MCP work).
    (void)stdin_inputs;

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE hReadPipe, hWritePipe;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) {
        return ToolResult{"[Error] Failed to create pipe.", false};
    }
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;

    PROCESS_INFORMATION pi = {};

    std::string full_cmd = "cmd.exe /c " + command;
    const char* cwd_ptr = cwd.empty() ? nullptr : cwd.c_str();

    BOOL ok = CreateProcessA(
        nullptr,
        const_cast<char*>(full_cmd.c_str()),
        nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        cwd_ptr,
        &si, &pi
    );

    CloseHandle(hWritePipe);

    if (!ok) {
        CloseHandle(hReadPipe);
        return ToolResult{"[Error] Failed to execute command.", false};
    }

    char buffer[4096];
    DWORD bytes_read;
    auto start = std::chrono::steady_clock::now();

    while (true) {
        DWORD avail = 0;
        PeekNamedPipe(hReadPipe, nullptr, 0, nullptr, &avail, nullptr);
        if (avail > 0) {
            if (ReadFile(hReadPipe, buffer, sizeof(buffer), &bytes_read, nullptr) && bytes_read > 0) {
                process_raw(buffer, bytes_read);
            }
        }

        DWORD wait_result = WaitForSingleObject(pi.hProcess, 0);
        if (wait_result == WAIT_OBJECT_0) {
            // Process finished - drain remaining output
            while (ReadFile(hReadPipe, buffer, sizeof(buffer), &bytes_read, nullptr) && bytes_read > 0) {
                process_raw(buffer, bytes_read);
            }
            break;
        }

        // Abort check
        if (ctx.abort_flag && ctx.abort_flag->load()) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 1000);
            while (PeekNamedPipe(hReadPipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
                if (ReadFile(hReadPipe, buffer, sizeof(buffer), &bytes_read, nullptr) && bytes_read > 0) {
                    process_raw(buffer, bytes_read);
                } else break;
            }
            aborted = true;
            break;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= timeout_ms) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 1000);
            while (PeekNamedPipe(hReadPipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
                if (ReadFile(hReadPipe, buffer, sizeof(buffer), &bytes_read, nullptr) && bytes_read > 0) {
                    process_raw(buffer, bytes_read);
                } else break;
            }
            timed_out = true;
            break;
        }

        Sleep(10);
    }

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hReadPipe);

#else
    // POSIX: fork/exec with streaming, stdin injection, and abort support.
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        return ToolResult{"[Error] Failed to create pipe.", false};
    }

    // stdin pipe (read by child, written by parent). Always created so the
    // child has a well-defined stdin; if stdin_inputs is empty we close the
    // write end immediately so the child sees EOF.
    int stdin_pipefd[2];
    if (pipe(stdin_pipefd) == -1) {
        close(pipefd[0]); close(pipefd[1]);
        return ToolResult{"[Error] Failed to create stdin pipe.", false};
    }

    pid_t pid = fork();
    if (pid == -1) {
        close(pipefd[0]); close(pipefd[1]);
        close(stdin_pipefd[0]); close(stdin_pipefd[1]);
        return ToolResult{"[Error] Failed to fork.", false};
    }

    if (pid == 0) {
        // Child
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        close(stdin_pipefd[1]);
        dup2(stdin_pipefd[0], STDIN_FILENO);
        close(stdin_pipefd[0]);

        // New process group so parent can signal the whole tree
        setpgid(0, 0);

        if (!cwd.empty()) {
            if (chdir(cwd.c_str()) != 0) {
                _exit(127);
            }
        }

        execl("/bin/sh", "sh", "-c", command.c_str(), nullptr);
        _exit(127);
    }

    // Parent
    close(pipefd[1]);
    close(stdin_pipefd[0]);

    // stdin writer thread (only if we have inputs to send)
    std::thread stdin_writer;
    if (!stdin_inputs.empty()) {
        int write_fd = stdin_pipefd[1];
        stdin_writer = std::thread([write_fd, inputs = stdin_inputs]() {
            for (const auto& line : inputs) {
                std::string data = line + "\n";
                const char* p = data.c_str();
                size_t remaining = data.size();
                while (remaining > 0) {
                    ssize_t n = write(write_fd, p, remaining);
                    if (n < 0) {
                        if (errno == EINTR) continue;
                        return; // pipe closed (child exited) or other error
                    }
                    p += n;
                    remaining -= n;
                }
            }
            close(write_fd);
        });
    } else {
        // No inputs: close immediately so child sees EOF on stdin
        close(stdin_pipefd[1]);
    }

    char buffer[4096];
    ssize_t n;
    auto start = std::chrono::steady_clock::now();

    int flags = fcntl(pipefd[0], F_GETFL, 0);
    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    int status = 0;
    while (true) {
        n = read(pipefd[0], buffer, sizeof(buffer));
        if (n > 0) {
            process_raw(buffer, n);
        }

        int wr = waitpid(pid, &status, WNOHANG);
        if (wr == pid) {
            while ((n = read(pipefd[0], buffer, sizeof(buffer))) > 0) {
                process_raw(buffer, n);
            }
            break;
        }

        if (ctx.abort_flag && ctx.abort_flag->load()) {
            kill(-pid, SIGTERM);
            // Give it up to 500ms to exit gracefully
            for (int i = 0; i < 50; ++i) {
                if (waitpid(pid, &status, WNOHANG) == pid) { aborted = true; break; }
                usleep(10000);
            }
            if (!aborted) {
                kill(-pid, SIGKILL);
                waitpid(pid, &status, 0);
                aborted = true;
            }
            while ((n = read(pipefd[0], buffer, sizeof(buffer))) > 0) {
                process_raw(buffer, n);
            }
            break;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= timeout_ms) {
            kill(-pid, SIGKILL);
            waitpid(pid, &status, 0);
            timed_out = true;
            while ((n = read(pipefd[0], buffer, sizeof(buffer))) > 0) {
                process_raw(buffer, n);
            }
            break;
        }

        usleep(10000); // 10ms
    }

    close(pipefd[0]);
    // If writer thread still running, close the write end to make it exit
    // (the child is dead; further writes would EPIPE).
    if (stdin_writer.joinable()) {
        // Not safe to close twice — the thread itself closes after finishing.
        stdin_writer.join();
    }

    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif

    // Flush any trailing partial line into full_output and tail_lines so it's
    // visible even without a trailing newline.
    if (!current_line.empty()) {
        full_output += current_line;
        tail_lines.push_back(current_line);
        while (tail_lines.size() > 5) tail_lines.pop_front();
        current_line.clear();
    }

    const size_t raw_bytes = full_output.size();

    // Head+tail truncation: keep first 40% and last 60% of MAX_OUTPUT_SIZE,
    // joined by a one-line marker reporting the omitted byte count. This
    // preserves early context (e.g. build args, cwd, path) which a pure
    // tail-only policy loses.
    bool was_truncated = false;
    if (full_output.size() > MAX_OUTPUT_SIZE) {
        const size_t head_cap = static_cast<size_t>(MAX_OUTPUT_SIZE * 0.4);
        const size_t tail_cap = MAX_OUTPUT_SIZE - head_cap; // ~60%
        const size_t omitted = full_output.size() - head_cap - tail_cap;
        std::string head = full_output.substr(0, head_cap);
        std::string tail = full_output.substr(full_output.size() - tail_cap);
        // Ensure the marker sits on its own line.
        if (!head.empty() && head.back() != '\n') head += "\n";
        full_output = head + "[... " + std::to_string(omitted) + " bytes omitted ...]\n" + tail;
        was_truncated = true;
    }

    full_output = ensure_utf8(full_output);

    auto t_end = std::chrono::steady_clock::now();
    long long duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        t_end - t_start).count();

    if (aborted) {
        if (full_output.empty() || full_output.back() != '\n') full_output += "\n";
        full_output += "[Aborted]";
        ToolResult r{full_output, false};
        r.summary = make_summary(command, duration_ms, raw_bytes, -1, false,
                                 was_truncated, true, false);
        return r;
    }
    if (timed_out) {
        ToolResult r{full_output + "\n[Error] Command timed out after " +
            std::to_string(timeout_ms / 1000) + " seconds.", false};
        r.summary = make_summary(command, duration_ms, raw_bytes, -1, false,
                                 was_truncated, false, true);
        return r;
    }

    if (full_output.empty()) {
        full_output = "(no output)";
    }

    bool is_ok = (exit_code == 0);
    ToolResult r{full_output, is_ok};
    r.summary = make_summary(command, duration_ms, raw_bytes,
                             static_cast<int>(exit_code), is_ok,
                             was_truncated, false, false);
    return r;
}

ToolImpl create_bash_tool() {
    ToolDef def;
    def.name = "bash";
    def.description = "Execute a shell command and return its output. "
                      "Use this to run commands, check files, install packages, etc. "
                      "For programs that prompt for input (e.g. 'apt install' confirming, "
                      "'npm login' asking for credentials), pass stdin_inputs with the "
                      "answers to pipe into the command's stdin in order.";
    def.parameters = nlohmann::json({
        {"type", "object"},
        {"properties", {
            {"command", {
                {"type", "string"},
                {"description", "The shell command to execute"}
            }},
            {"timeout_ms", {
                {"type", "integer"},
                {"description", "Timeout in milliseconds (default: 120000)"}
            }},
            {"cwd", {
                {"type", "string"},
                {"description", "Working directory for the command (default: agent CWD)"}
            }},
            {"stdin_inputs", {
                {"type", "array"},
                {"items", {{"type", "string"}}},
                {"description", "Optional: lines to feed to the command's stdin in order, "
                                "each followed by a newline. Use for programs that prompt "
                                "interactively, e.g. [\"y\"] for an 'apt install' confirmation. "
                                "On Windows this parameter is currently accepted but ignored."}
            }}
        }},
        {"required", nlohmann::json::array({"command"})}
    });

    return ToolImpl{def, execute_bash, /*is_read_only=*/false};
}

} // namespace acecode
