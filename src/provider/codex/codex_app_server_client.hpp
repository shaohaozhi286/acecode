#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace acecode::codex {

struct AccountInfo {
    bool present = false;
    std::string type;
    std::string email;
    std::string plan_type;
    bool requires_openai_auth = false;
};

struct LoginStartInfo {
    std::string type;
    std::string login_id;
    std::string user_code;
    std::string verification_url;
    std::string auth_url;
};

struct ModelInfo {
    std::string id;
    std::string model;
    std::string display_name;
    std::string description;
    bool hidden = false;
    bool is_default = false;
};

class AppServerClient {
public:
    using NotificationHandler =
        std::function<void(const std::string& method, const nlohmann::json& params)>;

    AppServerClient();
    ~AppServerClient();

    AppServerClient(const AppServerClient&) = delete;
    AppServerClient& operator=(const AppServerClient&) = delete;

    bool start(std::string* error = nullptr);
    void stop();
    bool running() const { return running_.load(); }

    void set_notification_handler(NotificationHandler handler);

    bool initialize(std::string* error = nullptr);
    std::optional<AccountInfo> read_account(bool refresh, std::string* error = nullptr);
    std::optional<LoginStartInfo> start_device_login(std::string* error = nullptr);
    std::vector<ModelInfo> list_models(bool include_hidden, std::string* error = nullptr);
    std::optional<std::string> start_thread(const std::string& model,
                                            const std::string& cwd,
                                            std::string* error = nullptr);
    std::optional<std::string> start_turn(const std::string& thread_id,
                                          const std::string& model,
                                          const std::string& cwd,
                                          const std::string& input_text,
                                          std::string* error = nullptr);

    std::optional<nlohmann::json> request(
        const std::string& method,
        const nlohmann::json& params,
        std::chrono::milliseconds timeout,
        std::string* error = nullptr);

private:
    struct PendingResponse {
        bool done = false;
        nlohmann::json result;
        nlohmann::json error;
    };

    bool start_process(std::string* error);
    void stop_process();
    void read_loop();
    void handle_line(const std::string& line);
    void handle_server_request(const nlohmann::json& message);
    bool write_json_line(const nlohmann::json& message, std::string* error);

    std::atomic<bool> running_{false};
    std::thread reader_;
    std::mutex write_mu_;
    std::mutex responses_mu_;
    std::condition_variable responses_cv_;
    std::map<std::int64_t, PendingResponse> responses_;
    std::atomic<std::int64_t> next_id_{1};
    NotificationHandler notification_handler_;

#ifdef _WIN32
    void* process_handle_ = nullptr;
    void* stdin_write_ = nullptr;
    void* stdout_read_ = nullptr;
#else
    int process_id_ = -1;
    int stdin_write_ = -1;
    int stdout_read_ = -1;
#endif
};

std::string describe_app_server_error(const nlohmann::json& error);

} // namespace acecode::codex
