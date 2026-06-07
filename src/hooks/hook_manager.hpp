#pragma once

#include "hook_config.hpp"
#include "hook_runner.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

namespace acecode {

using HookProcessRunner = std::function<HookProcessResult(
    const HookCommandSpec& command,
    const std::string& stdin_text,
    int timeout_ms,
    const std::string& cwd)>;

std::size_t dispatch_startup_before_model_load_hooks(const std::string& cwd,
                                                     std::string* error = nullptr);

class HookManager {
public:
    HookManager();
    explicit HookManager(HookConfig config, HookProcessRunner runner = HookProcessRunner{});
    ~HookManager();

    HookManager(const HookManager&) = delete;
    HookManager& operator=(const HookManager&) = delete;

    std::size_t dispatch(const std::string& event,
                         const nlohmann::json& payload,
                         const std::string& cwd);

    void shutdown(std::chrono::milliseconds wait_timeout = std::chrono::milliseconds(1000));

    const HookConfig& config() const { return config_; }

private:
    struct Invocation {
        HookDefinition hook;
        HookCommandSpec command;
        nlohmann::json payload;
        std::string cwd;
    };

    struct AsyncState {
        std::mutex mu;
        std::condition_variable cv;
        std::condition_variable done_cv;
        std::deque<Invocation> queue;
        bool stopping = false;
        bool active = false;
        bool done = false;
    };

    void start_worker_locked();
    void enqueue_async(Invocation invocation);
    void run_invocation(const Invocation& invocation) const;

    static void worker_loop(std::shared_ptr<AsyncState> state,
                            HookProcessRunner runner);
    static void run_invocation_with_runner(const Invocation& invocation,
                                           const HookProcessRunner& runner);

    HookConfig config_;
    HookProcessRunner runner_;
    std::shared_ptr<AsyncState> async_state_;
    std::thread worker_;
    bool worker_detached_ = false;
};

} // namespace acecode
