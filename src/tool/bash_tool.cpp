#include "bash_tool.hpp"
#include "utils/logger.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <chrono>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#endif

namespace acecode {

static constexpr int DEFAULT_TIMEOUT_MS = 120000;    // 2 minutes
static constexpr size_t MAX_OUTPUT_SIZE = 100 * 1024; // 100KB

static ToolResult execute_bash(const std::string& arguments_json) {
    std::string command;
    int timeout_ms = DEFAULT_TIMEOUT_MS;
    std::string cwd;

    try {
        auto args = nlohmann::json::parse(arguments_json);
        command = args.value("command", "");
        timeout_ms = args.value("timeout_ms", DEFAULT_TIMEOUT_MS);
        cwd = args.value("cwd", "");
    } catch (...) {
        return ToolResult{"[Error] Failed to parse tool arguments.", false};
    }

    if (command.empty()) {
        return ToolResult{"[Error] No command provided.", false};
    }

    LOG_INFO("bash: cmd=" + log_truncate(command, 200) + " cwd=" + cwd + " timeout=" + std::to_string(timeout_ms));

    // Validate cwd if provided
    if (!cwd.empty() && !std::filesystem::is_directory(cwd)) {
        return ToolResult{"[Error] Working directory does not exist: " + cwd, false};
    }

#ifdef _WIN32
    // Windows: CreateProcess for timeout support
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

    // Read output while waiting
    std::string output;
    char buffer[4096];
    DWORD bytes_read;

    // Set pipe to non-blocking for timeout checking
    DWORD wait_result = WAIT_TIMEOUT;
    auto start = std::chrono::steady_clock::now();

    while (true) {
        DWORD avail = 0;
        PeekNamedPipe(hReadPipe, nullptr, 0, nullptr, &avail, nullptr);
        if (avail > 0) {
            if (ReadFile(hReadPipe, buffer, sizeof(buffer), &bytes_read, nullptr) && bytes_read > 0) {
                output.append(buffer, bytes_read);
            }
        }

        wait_result = WaitForSingleObject(pi.hProcess, 0);
        if (wait_result == WAIT_OBJECT_0) {
            // Process finished - read remaining output
            while (ReadFile(hReadPipe, buffer, sizeof(buffer), &bytes_read, nullptr) && bytes_read > 0) {
                output.append(buffer, bytes_read);
            }
            break;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= timeout_ms) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 1000);
            // Read remaining output
            while (PeekNamedPipe(hReadPipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
                if (ReadFile(hReadPipe, buffer, sizeof(buffer), &bytes_read, nullptr) && bytes_read > 0) {
                    output.append(buffer, bytes_read);
                } else {
                    break;
                }
            }
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            CloseHandle(hReadPipe);

            // Truncate if needed
            if (output.size() > MAX_OUTPUT_SIZE) {
                output = "[Output truncated, showing last 100KB]\n" +
                    output.substr(output.size() - MAX_OUTPUT_SIZE);
            }
            return ToolResult{output + "\n[Error] Command timed out after " +
                std::to_string(timeout_ms / 1000) + " seconds.", false};
        }

        Sleep(10);
    }

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hReadPipe);

#else
    // POSIX: fork/exec with timeout
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        return ToolResult{"[Error] Failed to create pipe.", false};
    }

    pid_t pid = fork();
    if (pid == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        return ToolResult{"[Error] Failed to fork.", false};
    }

    if (pid == 0) {
        // Child
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

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
    std::string output;
    char buffer[4096];
    ssize_t n;
    auto start = std::chrono::steady_clock::now();

    // Set read end non-blocking
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    bool timed_out = false;
    int status = 0;
    while (true) {
        n = read(pipefd[0], buffer, sizeof(buffer));
        if (n > 0) {
            output.append(buffer, n);
        }

        int wr = waitpid(pid, &status, WNOHANG);
        if (wr == pid) {
            // Process finished - drain remaining output
            while ((n = read(pipefd[0], buffer, sizeof(buffer))) > 0) {
                output.append(buffer, n);
            }
            break;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= timeout_ms) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            timed_out = true;
            break;
        }

        usleep(10000); // 10ms
    }

    close(pipefd[0]);
    int exit_code = WEXITSTATUS(status);

    if (timed_out) {
        if (output.size() > MAX_OUTPUT_SIZE) {
            output = "[Output truncated, showing last 100KB]\n" +
                output.substr(output.size() - MAX_OUTPUT_SIZE);
        }
        return ToolResult{output + "\n[Error] Command timed out after " +
            std::to_string(timeout_ms / 1000) + " seconds.", false};
    }
#endif

    // Truncate output if too large
    if (output.size() > MAX_OUTPUT_SIZE) {
        output = "[Output truncated, showing last 100KB]\n" +
            output.substr(output.size() - MAX_OUTPUT_SIZE);
    }

    if (output.empty()) {
        output = "(no output)";
    }

    return ToolResult{output, exit_code == 0};
}

ToolImpl create_bash_tool() {
    ToolDef def;
    def.name = "bash";
    def.description = "Execute a shell command and return its output. "
                      "Use this to run commands, check files, install packages, etc.";
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
            }}
        }},
        {"required", nlohmann::json::array({"command"})}
    });

    return ToolImpl{def, execute_bash};
}

} // namespace acecode
