#include "bash_tool.hpp"
#include "tool_icons.hpp"
#include "utils/logger.hpp"
#include "utils/encoding.hpp"
#include "utils/stream_processing.hpp"
#include "utils/utf8_path.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <deque>
#include <chrono>
#include <filesystem>
#include <thread>
#include <atomic>
#include <set>
#include <system_error>
#include <cstdlib>

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

static std::string ascii_lower_copy(std::string value) {
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return value;
}

static bool is_root_script_extension(const std::filesystem::path& path) {
    std::string ext = ascii_lower_copy(path_to_utf8(path.extension()));
    return ext == ".py" || ext == ".ps1" || ext == ".js" ||
           ext == ".bat" || ext == ".cmd";
}

static std::set<std::string> list_root_script_files(const std::filesystem::path& root) {
    std::set<std::string> out;
    std::error_code ec;
    if (root.empty() || !std::filesystem::is_directory(root, ec) || ec) return out;
    for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
        if (ec) break;
        if (!entry.is_regular_file(ec) || ec) {
            ec.clear();
            continue;
        }
        const auto path = entry.path();
        if (is_root_script_extension(path)) {
            out.insert(path_to_utf8(path.filename()));
        }
    }
    return out;
}

static std::vector<std::string> new_root_script_files(
    const std::set<std::string>& before,
    const std::set<std::string>& after) {
    std::vector<std::string> created;
    for (const auto& name : after) {
        if (before.find(name) == before.end()) created.push_back(name);
    }
    return created;
}

static void append_root_script_warning(ToolResult& result,
                                       const std::vector<std::string>& created,
                                       const std::string& scratch_dir) {
    if (created.empty()) return;
    if (!result.output.empty() && result.output.back() != '\n') result.output += "\n";
    result.output += "[Warning] Shell command created script file(s) in the workspace root: ";
    for (size_t i = 0; i < created.size(); ++i) {
        if (i > 0) result.output += ", ";
        result.output += created[i];
    }
    result.output += ". Temporary helper scripts should be written under ACECODE_TMPDIR";
    if (!scratch_dir.empty()) result.output += " (" + scratch_dir + ")";
    result.output += ".";
}

#ifdef _WIN32
static std::wstring env_name_prefix(const std::wstring& name) {
    return name + L"=";
}

static bool starts_with_env_name_ci(const std::wstring& value,
                                    const std::wstring& prefix) {
    if (value.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        wchar_t a = value[i];
        wchar_t b = prefix[i];
        if (a >= L'A' && a <= L'Z') a = static_cast<wchar_t>(a - L'A' + L'a');
        if (b >= L'A' && b <= L'Z') b = static_cast<wchar_t>(b - L'A' + L'a');
        if (a != b) return false;
    }
    return true;
}

static std::vector<wchar_t> build_environment_block_with_var(
    const std::wstring& name,
    const std::wstring& value) {
    std::vector<std::wstring> entries;
    const std::wstring prefix = env_name_prefix(name);
    LPWCH env = GetEnvironmentStringsW();
    if (env) {
        for (LPWCH p = env; *p != L'\0'; ) {
            std::wstring entry(p);
            const size_t entry_len = entry.size();
            if (!starts_with_env_name_ci(entry, prefix)) {
                entries.push_back(std::move(entry));
            }
            p += entry_len + 1;
        }
        FreeEnvironmentStringsW(env);
    }
    entries.push_back(prefix + value);

    std::vector<wchar_t> block;
    for (const auto& entry : entries) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}
#endif

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

    if (cwd.empty() && !ctx.cwd.empty()) {
        cwd = ctx.cwd;
    }

    auto t_start = std::chrono::steady_clock::now();
    auto make_summary = [&](const std::string& cmd, long long duration_ms,
                            size_t total_bytes_out, int exit_code,
                            bool is_success, bool was_truncated,
                            bool was_aborted, bool was_timed_out) {
        ToolSummary s;
        s.verb = "Ran";
        s.object = truncate_utf8_prefix(cmd, 60);
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

#ifdef _WIN32
    std::filesystem::path cwd_path = cwd.empty()
        ? std::filesystem::path{}
        : std::filesystem::path(utf8_to_wide(cwd));
#else
    std::filesystem::path cwd_path = cwd;
#endif

    if (!cwd.empty() && !std::filesystem::is_directory(cwd_path)) {
        ToolResult r{"[Error] Working directory does not exist: " + cwd, false};
        r.summary = make_summary(command, 0, 0, -1, false, false, false, false);
        return r;
    }

    std::error_code cwd_ec;
    const std::filesystem::path root_for_script_warning = cwd_path.empty()
        ? std::filesystem::current_path(cwd_ec)
        : cwd_path;
    const auto root_scripts_before = list_root_script_files(root_for_script_warning);

    std::string scratch_dir = ctx.scratch_dir;
    if (!scratch_dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(path_from_utf8(scratch_dir), ec);
        if (ec) {
            ToolResult r{"[Error] Failed to create ACECODE_TMPDIR: " + scratch_dir +
                         " (" + ec.message() + ")", false};
            r.summary = make_summary(command, 0, 0, -1, false, false, false, false);
            return r;
        }
    }

    // Shared streaming state across both OS branches.
    std::string full_output;
    std::string current_line;
    std::deque<std::string> tail_lines;
    int total_lines = 0;

    // Incremental decoder: turns raw subprocess bytes into valid UTF-8, holding
    // back any trailing partial character across chunk boundaries. On Windows it
    // falls back to the console codepage (e.g. GBK/CP936) so legacy cmd.exe
    // output decodes correctly instead of corrupting JSON serialization with a
    // stray byte like 0xF7. Output here is always safe to stream and to embed in
    // the tool result JSON.
    IncrementalTextDecoder decoder;

    // Emit already-decoded UTF-8 text into the output/stream pipeline.
    auto emit_text = [&](std::string text) {
        if (text.empty()) return;
#ifdef _WIN32
        text = normalize_crlf(text);
#endif
        std::string clean = strip_ansi(text);
        if (clean.empty()) return;
        full_output += clean;
        feed_line_state(clean, current_line, tail_lines, total_lines);
        if (ctx.stream) ctx.stream(clean);
    };

    auto process_raw = [&](const char* data, size_t len) {
        if (len == 0) return;
        emit_text(decoder.push(data, len));
    };

    // Drain any bytes the decoder is still holding (called once after EOF).
    auto flush_decoder = [&]() {
        emit_text(decoder.flush());
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

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;

    PROCESS_INFORMATION pi = {};

    std::wstring full_cmd = L"cmd.exe /c " + utf8_to_wide(command);
    std::vector<wchar_t> full_cmd_buffer(full_cmd.begin(), full_cmd.end());
    full_cmd_buffer.push_back(L'\0');

    std::wstring wide_cwd;
    if (!cwd.empty()) {
        wide_cwd = utf8_to_wide(cwd);
    }
    const wchar_t* cwd_ptr = wide_cwd.empty() ? nullptr : wide_cwd.c_str();

    std::vector<wchar_t> env_block;
    void* env_ptr = nullptr;
    if (!scratch_dir.empty()) {
        env_block = build_environment_block_with_var(
            L"ACECODE_TMPDIR", utf8_to_wide(scratch_dir));
        env_ptr = env_block.data();
    }

    BOOL ok = CreateProcessW(
        nullptr,
        full_cmd_buffer.data(),
        nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | (env_ptr ? CREATE_UNICODE_ENVIRONMENT : 0),
        env_ptr,
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
            // Process finished - drain remaining output.
            // **不能**裸 ReadFile:Windows 管道在同步模式下,只要还有任何进程
            // 持有写端,ReadFile 在数据耗尽时会**永久阻塞**。直接子进程(cmd.exe)
            // 已退出,但孙进程(典型场景:`agent-browser open` 启动的 Chrome,
            // 后台 daemon,detached 的 powershell 子任务……)如果继承了同一根管道,
            // 仍 hold 着写端,ReadFile 永远等不到 EOF。这条循环之外才有 abort /
            // timeout 检查,一旦在这里 block 住,Esc / Ctrl+C / 120s 超时全部失效,
            // TUI 表现为"卡死"。修复:沿用下方 abort / timeout drain 一样的模式,
            // 只 drain 当前可读字节,管道里没数据立刻 break,把后台孙进程的命运
            // 交给 OS。
            while (true) {
                DWORD drain_avail = 0;
                if (!PeekNamedPipe(hReadPipe, nullptr, 0, nullptr, &drain_avail, nullptr)) break;
                if (drain_avail == 0) break;
                if (!ReadFile(hReadPipe, buffer, sizeof(buffer), &bytes_read, nullptr)) break;
                if (bytes_read == 0) break;
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
        if (!scratch_dir.empty()) {
            setenv("ACECODE_TMPDIR", scratch_dir.c_str(), 1);
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

    // EOF reached: drain any bytes the decoder held back (incomplete trailing
    // character or undecodable tail) so they are not silently dropped.
    flush_decoder();

    // Flush any trailing partial line into full_output and tail_lines so it's
    // visible even without a trailing newline.
    if (!current_line.empty()) {
        full_output += current_line;
        tail_lines.push_back(current_line);
        while (tail_lines.size() > 5) tail_lines.pop_front();
        current_line.clear();
    }

    const size_t raw_bytes = full_output.size();

    // AgentLoop 会把大结果交给 tool_result_storage 落盘并生成 preview,所以
    // 模型驱动路径必须保留完整输出;直接单测/嵌入式调用默认仍走旧的 100KB 保护。
    const bool should_truncate_inline = !ctx.preserve_full_output;

    // Head+tail truncation: keep first 40% and last 60% of MAX_OUTPUT_SIZE,
    // joined by a one-line marker reporting the omitted byte count. This
    // preserves early context (e.g. build args, cwd, path) which a pure
    // tail-only policy loses.
    bool was_truncated = false;
    if (should_truncate_inline && full_output.size() > MAX_OUTPUT_SIZE) {
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
        append_root_script_warning(r,
            new_root_script_files(root_scripts_before,
                                  list_root_script_files(root_for_script_warning)),
            scratch_dir);
        return r;
    }
    if (timed_out) {
        ToolResult r{full_output + "\n[Error] Command timed out after " +
            std::to_string(timeout_ms / 1000) + " seconds.", false};
        r.summary = make_summary(command, duration_ms, raw_bytes, -1, false,
                                 was_truncated, false, true);
        append_root_script_warning(r,
            new_root_script_files(root_scripts_before,
                                  list_root_script_files(root_for_script_warning)),
            scratch_dir);
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
    append_root_script_warning(r,
        new_root_script_files(root_scripts_before,
                              list_root_script_files(root_for_script_warning)),
        scratch_dir);
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
