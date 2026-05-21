#include "codex_app_server_client.hpp"
#include "codex_model_catalog.hpp"

#include "../utils/logger.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

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
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace acecode::codex {
namespace {

constexpr auto kDefaultRequestTimeout = std::chrono::seconds(60);

std::string json_value_string(const nlohmann::json& object, const char* key) {
    if (!object.is_object()) return {};
    auto it = object.find(key);
    if (it == object.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

bool json_value_bool(const nlohmann::json& object, const char* key, bool fallback = false) {
    if (!object.is_object()) return fallback;
    auto it = object.find(key);
    if (it == object.end() || !it->is_boolean()) return fallback;
    return it->get<bool>();
}

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string strip_provider_prefix(const std::string& model) {
    const auto pos = model.find('/');
    if (pos == std::string::npos || pos + 1 >= model.size()) return model;
    return model.substr(pos + 1);
}

#ifdef _WIN32
std::string windows_error_message(DWORD code) {
    LPSTR buffer = nullptr;
    DWORD size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&buffer),
        0,
        nullptr);
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
#else
void close_fd(int& fd) {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}
#endif

} // namespace

AppServerClient::AppServerClient() = default;

AppServerClient::~AppServerClient() {
    stop();
}

void AppServerClient::set_notification_handler(NotificationHandler handler) {
    std::lock_guard<std::mutex> lk(responses_mu_);
    notification_handler_ = std::move(handler);
}

bool AppServerClient::start(std::string* error) {
    if (running_.load()) return true;
    if (reader_.joinable()) {
        stop_process();
        reader_.join();
    }
    if (!start_process(error)) return false;
    running_.store(true);
    reader_ = std::thread(&AppServerClient::read_loop, this);
    return true;
}

void AppServerClient::stop() {
    running_.store(false);
    stop_process();
    if (reader_.joinable()) reader_.join();
    {
        std::lock_guard<std::mutex> lk(responses_mu_);
        for (auto& [id, pending] : responses_) {
            pending.done = true;
            pending.error = {
                {"code", -32000},
                {"message", "Codex app-server stopped"}
            };
        }
    }
    responses_cv_.notify_all();
}

bool AppServerClient::initialize(std::string* error) {
    nlohmann::json params = {
        {"clientInfo", {
            {"name", "acecode"},
            {"version", "0.0.0"},
            {"title", "ACECode"}
        }},
        {"capabilities", {
            {"experimentalApi", true},
            {"optOutNotificationMethods", nlohmann::json::array()}
        }}
    };
    return request("initialize", params, kDefaultRequestTimeout, error).has_value();
}

std::optional<AccountInfo> AppServerClient::read_account(bool refresh, std::string* error) {
    auto result = request(
        "account/read",
        nlohmann::json{{"refresh", refresh}},
        kDefaultRequestTimeout,
        error);
    if (!result.has_value()) return std::nullopt;

    AccountInfo out;
    out.requires_openai_auth = json_value_bool(*result, "requiresOpenaiAuth", false);
    if (result->contains("account") && !(*result)["account"].is_null() &&
        (*result)["account"].is_object()) {
        const auto& account = (*result)["account"];
        out.present = true;
        out.type = json_value_string(account, "type");
        out.email = json_value_string(account, "email");
        out.plan_type = json_value_string(account, "planType");
    }
    return out;
}

std::optional<LoginStartInfo> AppServerClient::start_device_login(std::string* error) {
    auto result = request(
        "account/login/start",
        nlohmann::json{{"type", "chatgptDeviceCode"}},
        kDefaultRequestTimeout,
        error);
    if (!result.has_value()) return std::nullopt;

    LoginStartInfo out;
    out.type = json_value_string(*result, "type");
    out.login_id = json_value_string(*result, "loginId");
    out.user_code = json_value_string(*result, "userCode");
    out.verification_url = json_value_string(*result, "verificationUrl");
    out.auth_url = json_value_string(*result, "authUrl");
    return out;
}

int context_window_for_model(const std::string& model) {
    const std::string normalized = strip_provider_prefix(lower_ascii(model));
    if (normalized == "gpt-5.3-codex-spark") return 128000;
    if (normalized == "gpt-5.5" ||
        normalized == "gpt-5.4" ||
        normalized == "gpt-5.4-mini" ||
        normalized == "gpt-5.3-codex" ||
        normalized == "gpt-5.2" ||
        normalized == "codex-auto-review") {
        return 272000;
    }
    return 0;
}

std::vector<ModelInfo> AppServerClient::list_models(bool include_hidden, std::string* error) {
    nlohmann::json params = nlohmann::json::object();
    if (include_hidden) params["includeHidden"] = true;
    auto result = request("model/list", params, kDefaultRequestTimeout, error);
    if (!result.has_value()) return {};

    std::vector<ModelInfo> out;
    if (!result->contains("data") || !(*result)["data"].is_array()) return out;
    for (const auto& item : (*result)["data"]) {
        if (!item.is_object()) continue;
        ModelInfo model;
        model.id = json_value_string(item, "id");
        model.model = json_value_string(item, "model");
        model.display_name = json_value_string(item, "displayName");
        model.description = json_value_string(item, "description");
        model.hidden = json_value_bool(item, "hidden", false);
        model.is_default = json_value_bool(item, "isDefault", false);
        if (model.id.empty()) model.id = model.model;
        if (model.model.empty()) model.model = model.id;
        if (!model.id.empty()) out.push_back(std::move(model));
    }
    return out;
}

std::optional<std::string> AppServerClient::start_thread(const std::string& model,
                                                         const std::string& cwd,
                                                         std::string* error) {
    nlohmann::json params = {
        {"model", model.empty() ? nlohmann::json(nullptr) : nlohmann::json(model)},
        {"serviceName", "acecode"},
        {"ephemeral", true},
        {"approvalPolicy", "never"},
        {"sandbox", "workspace-write"}
    };
    if (!cwd.empty()) params["cwd"] = cwd;

    auto result = request("thread/start", params, kDefaultRequestTimeout, error);
    if (!result.has_value()) return std::nullopt;
    if (result->contains("thread") && (*result)["thread"].is_object()) {
        std::string id = json_value_string((*result)["thread"], "id");
        if (!id.empty()) return id;
    }
    if (error) *error = "Codex app-server thread/start response missing thread.id";
    return std::nullopt;
}

std::optional<std::string> AppServerClient::start_turn(const std::string& thread_id,
                                                       const std::string& model,
                                                       const std::string& cwd,
                                                       const std::string& input_text,
                                                       std::string* error) {
    nlohmann::json params = {
        {"threadId", thread_id},
        {"input", nlohmann::json::array({
            nlohmann::json{{"type", "text"}, {"text", input_text}}
        })},
        {"approvalPolicy", "never"},
        {"sandboxPolicy", {
            {"type", "workspaceWrite"},
            {"networkAccess", true}
        }}
    };
    if (!model.empty()) params["model"] = model;
    if (!cwd.empty()) params["cwd"] = cwd;

    auto result = request("turn/start", params, kDefaultRequestTimeout, error);
    if (!result.has_value()) return std::nullopt;
    if (result->contains("turn") && (*result)["turn"].is_object()) {
        std::string id = json_value_string((*result)["turn"], "id");
        if (!id.empty()) return id;
    }
    if (error) *error = "Codex app-server turn/start response missing turn.id";
    return std::nullopt;
}

std::optional<nlohmann::json> AppServerClient::request(
    const std::string& method,
    const nlohmann::json& params,
    std::chrono::milliseconds timeout,
    std::string* error) {
    if (!running_.load()) {
        if (!start(error)) return std::nullopt;
    }

    const std::int64_t id = next_id_.fetch_add(1);
    {
        std::lock_guard<std::mutex> lk(responses_mu_);
        responses_[id] = PendingResponse{};
    }

    nlohmann::json message = {
        {"id", id},
        {"method", method},
        {"params", params.is_null() ? nlohmann::json::object() : params}
    };
    if (!write_json_line(message, error)) {
        std::lock_guard<std::mutex> lk(responses_mu_);
        responses_.erase(id);
        return std::nullopt;
    }

    std::unique_lock<std::mutex> lk(responses_mu_);
    bool ready = responses_cv_.wait_for(lk, timeout, [&] {
        auto it = responses_.find(id);
        return it == responses_.end() || it->second.done;
    });
    if (!ready) {
        responses_.erase(id);
        if (error) *error = "Timed out waiting for Codex app-server response to " + method;
        return std::nullopt;
    }

    auto it = responses_.find(id);
    if (it == responses_.end()) {
        if (error) *error = "Codex app-server response bookkeeping failed for " + method;
        return std::nullopt;
    }
    PendingResponse pending = std::move(it->second);
    responses_.erase(it);
    lk.unlock();

    if (!pending.error.is_null() && !pending.error.empty()) {
        if (error) *error = describe_app_server_error(pending.error);
        return std::nullopt;
    }
    return pending.result;
}

bool AppServerClient::write_json_line(const nlohmann::json& message, std::string* error) {
    const std::string line = message.dump() + "\n";
    std::lock_guard<std::mutex> lk(write_mu_);
#ifdef _WIN32
    DWORD written = 0;
    BOOL ok = WriteFile(static_cast<HANDLE>(stdin_write_),
                        line.data(),
                        static_cast<DWORD>(line.size()),
                        &written,
                        nullptr);
    if (!ok || written != line.size()) {
        if (error) *error = "Failed to write to Codex app-server: " +
            windows_error_message(GetLastError());
        return false;
    }
#else
    const char* data = line.data();
    std::size_t remaining = line.size();
    while (remaining > 0) {
        ssize_t n = write(stdin_write_, data, remaining);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (error) *error = std::string("Failed to write to Codex app-server: ") +
                std::strerror(errno);
            return false;
        }
        data += n;
        remaining -= static_cast<std::size_t>(n);
    }
#endif
    return true;
}

void AppServerClient::read_loop() {
    std::string buffer;
    char chunk[4096];
    while (running_.load()) {
#ifdef _WIN32
        DWORD bytes_read = 0;
        BOOL ok = ReadFile(static_cast<HANDLE>(stdout_read_),
                           chunk,
                           static_cast<DWORD>(sizeof(chunk)),
                           &bytes_read,
                           nullptr);
        if (!ok || bytes_read == 0) {
            if (running_.load()) {
                DWORD err = GetLastError();
                if (err != ERROR_BROKEN_PIPE && err != ERROR_HANDLE_EOF) {
                    LOG_WARN("[codex] app-server stdout read failed: " +
                             windows_error_message(err));
                }
            }
            break;
        }
#else
        ssize_t bytes_read = read(stdout_read_, chunk, sizeof(chunk));
        if (bytes_read < 0) {
            if (errno == EINTR) continue;
            if (running_.load()) {
                LOG_WARN(std::string("[codex] app-server stdout read failed: ") +
                         std::strerror(errno));
            }
            break;
        }
        if (bytes_read == 0) break;
#endif
        buffer.append(chunk, static_cast<std::size_t>(bytes_read));
        std::size_t pos = 0;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) handle_line(line);
        }
    }

    running_.store(false);
    {
        std::lock_guard<std::mutex> lk(responses_mu_);
        for (auto& [id, pending] : responses_) {
            if (!pending.done) {
                pending.done = true;
                pending.error = {
                    {"code", -32000},
                    {"message", "Codex app-server stdout closed"}
                };
            }
        }
    }
    responses_cv_.notify_all();
}

void AppServerClient::handle_line(const std::string& line) {
    nlohmann::json message;
    try {
        message = nlohmann::json::parse(line);
    } catch (const nlohmann::json::parse_error& e) {
        LOG_WARN(std::string("[codex] ignored malformed app-server JSONL: ") + e.what());
        return;
    }
    if (!message.is_object()) return;

    if (message.contains("id") && message.contains("method") &&
        !message.contains("result") && !message.contains("error")) {
        handle_server_request(message);
        return;
    }

    if (message.contains("id") &&
        (message.contains("result") || message.contains("error"))) {
        if (!message["id"].is_number_integer()) return;
        const std::int64_t id = message["id"].get<std::int64_t>();
        {
            std::lock_guard<std::mutex> lk(responses_mu_);
            auto it = responses_.find(id);
            if (it != responses_.end()) {
                it->second.done = true;
                if (message.contains("result")) it->second.result = message["result"];
                if (message.contains("error")) it->second.error = message["error"];
            }
        }
        responses_cv_.notify_all();
        return;
    }

    if (message.contains("method") && message["method"].is_string()) {
        NotificationHandler handler;
        {
            std::lock_guard<std::mutex> lk(responses_mu_);
            handler = notification_handler_;
        }
        if (handler) {
            handler(message["method"].get<std::string>(),
                    message.value("params", nlohmann::json::object()));
        }
    }
}

void AppServerClient::handle_server_request(const nlohmann::json& message) {
    if (!message["id"].is_number_integer()) return;
    nlohmann::json response = {
        {"id", message["id"]},
        {"error", {
            {"code", -32601},
            {"message", "ACECode Codex provider does not handle app-server client requests"}
        }}
    };
    std::string ignored;
    write_json_line(response, &ignored);
}

bool AppServerClient::start_process(std::string* error) {
#ifdef _WIN32
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

    child_stderr = CreateFileA("NUL",
                               GENERIC_WRITE,
                               FILE_SHARE_WRITE | FILE_SHARE_READ,
                               &sa,
                               OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL,
                               nullptr);
    if (child_stderr == INVALID_HANDLE_VALUE) {
        child_stderr = nullptr;
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = child_stdin_read;
    si.hStdOutput = child_stdout_write;
    si.hStdError = child_stderr ? child_stderr : child_stdout_write;

    PROCESS_INFORMATION pi{};
    std::string command = "cmd.exe /d /c codex app-server --listen stdio://";
    std::vector<char> mutable_command(command.begin(), command.end());
    mutable_command.push_back('\0');

    BOOL ok = CreateProcessA(nullptr,
                             mutable_command.data(),
                             nullptr,
                             nullptr,
                             TRUE,
                             CREATE_NO_WINDOW,
                             nullptr,
                             nullptr,
                             &si,
                             &pi);
    if (!ok) {
        if (error) *error = "Failed to start `codex app-server`: " +
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
        execlp("codex", "codex", "app-server", "--listen", "stdio://", nullptr);
        _exit(127);
    }

    close_fd(stdin_pipe[0]);
    close_fd(stdout_pipe[1]);
    process_id_ = pid;
    stdin_write_ = stdin_pipe[1];
    stdout_read_ = stdout_pipe[0];
    return true;
#endif
}

void AppServerClient::stop_process() {
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
        kill(process_id_, SIGTERM);
    }
    close_fd(stdin_write_);
    close_fd(stdout_read_);
    if (process_id_ > 0) {
        int status = 0;
        for (int i = 0; i < 20; ++i) {
            pid_t r = waitpid(process_id_, &status, WNOHANG);
            if (r == process_id_) break;
            if (r < 0 && errno != EINTR) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        kill(process_id_, SIGKILL);
        waitpid(process_id_, &status, 0);
        process_id_ = -1;
    }
#endif
}

std::string describe_app_server_error(const nlohmann::json& error) {
    if (error.is_object()) {
        std::string message = json_value_string(error, "message");
        if (!message.empty()) return message;
        return error.dump();
    }
    if (error.is_string()) return error.get<std::string>();
    return error.dump();
}

} // namespace acecode::codex
