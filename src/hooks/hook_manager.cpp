#include "hook_manager.hpp"

#include "hook_payload.hpp"
#include "../utils/encoding.hpp"
#include "../utils/logger.hpp"

#include <chrono>
#include <utility>

namespace acecode {
namespace {

HookProcessResult default_runner(const HookCommandSpec& command,
                                 const std::string& stdin_text,
                                 int timeout_ms,
                                 const std::string& cwd) {
    return run_hook_process(command, stdin_text, timeout_ms, cwd);
}

std::string mode_name(HookMode mode) {
    return mode == HookMode::Async ? "async" : "sync";
}

nlohmann::json payload_for_hook(const nlohmann::json& base,
                                const HookDefinition& hook) {
    nlohmann::json payload = base.is_object() ? base : nlohmann::json::object();
    payload["event"] = hook.event;
    payload["hook_id"] = hook.id;
    return payload;
}

} // namespace

HookManager::HookManager()
    : HookManager(HookConfig{}, HookProcessRunner{}) {}

HookManager::HookManager(HookConfig config, HookProcessRunner runner)
    : config_(std::move(config)),
      runner_(runner ? std::move(runner) : HookProcessRunner(default_runner)),
      async_state_(std::make_shared<AsyncState>()) {}

std::size_t dispatch_startup_before_model_load_hooks(const std::string& cwd,
                                                     std::string* error) {
    HookConfig hook_config = load_hook_config(error);
    HookManager hook_manager(std::move(hook_config));
    auto payload = build_startup_before_model_load_payload(cwd);
    return hook_manager.dispatch(kHookEventStartupBeforeModelLoad, payload, cwd);
}

HookManager::~HookManager() {
    shutdown(std::chrono::milliseconds(1000));
}

std::size_t HookManager::dispatch(const std::string& event,
                                  const nlohmann::json& payload,
                                  const std::string& cwd) {
    if (!config_.enabled) return 0;
    auto it = config_.events.find(event);
    if (it == config_.events.end()) return 0;

    std::size_t count = 0;
    HookPlatform current = current_hook_platform();
    for (const auto& hook : it->second) {
        if (!hook_platform_matches(hook, current)) {
            LOG_DEBUG("[hooks] skip id=" + hook.id + " event=" + event +
                      " platform=" + hook_platform_name(current));
            continue;
        }
        auto command = resolve_hook_command(hook, current);
        if (!command.has_value()) {
            LOG_WARN("[hooks] skip id=" + hook.id + " event=" + event +
                     " no command for platform=" + hook_platform_name(current));
            continue;
        }

        Invocation invocation;
        invocation.hook = hook;
        invocation.command = std::move(*command);
        invocation.payload = payload_for_hook(payload, hook);
        invocation.cwd = cwd;
        ++count;

        if (hook.mode == HookMode::Async) {
            enqueue_async(std::move(invocation));
        } else {
            run_invocation(invocation);
        }
    }
    return count;
}

void HookManager::shutdown(std::chrono::milliseconds wait_timeout) {
    if (!async_state_) return;
    {
        std::lock_guard<std::mutex> lk(async_state_->mu);
        async_state_->stopping = true;
    }
    async_state_->cv.notify_all();

    bool done = true;
    if (worker_.joinable()) {
        std::unique_lock<std::mutex> lk(async_state_->mu);
        done = async_state_->done_cv.wait_for(lk, wait_timeout, [&] {
            return async_state_->done;
        });
        lk.unlock();
        if (done) {
            worker_.join();
        } else if (!worker_detached_) {
            worker_.detach();
            worker_detached_ = true;
            LOG_WARN("[hooks] async worker still running during shutdown; detached");
        }
    }
}

void HookManager::start_worker_locked() {
    if (worker_.joinable() || worker_detached_) return;
    auto state = async_state_;
    auto runner = runner_;
    worker_ = std::thread([state, runner] {
        worker_loop(state, runner);
    });
}

void HookManager::enqueue_async(Invocation invocation) {
    {
        std::lock_guard<std::mutex> lk(async_state_->mu);
        start_worker_locked();
        async_state_->queue.push_back(std::move(invocation));
    }
    async_state_->cv.notify_one();
}

void HookManager::run_invocation(const Invocation& invocation) const {
    run_invocation_with_runner(invocation, runner_);
}

void HookManager::worker_loop(std::shared_ptr<AsyncState> state,
                              HookProcessRunner runner) {
    for (;;) {
        Invocation invocation;
        {
            std::unique_lock<std::mutex> lk(state->mu);
            state->cv.wait(lk, [&] {
                return state->stopping || !state->queue.empty();
            });
            if (state->queue.empty()) {
                if (state->stopping) {
                    state->done = true;
                    state->done_cv.notify_all();
                    return;
                }
                continue;
            }
            invocation = std::move(state->queue.front());
            state->queue.pop_front();
            state->active = true;
        }

        run_invocation_with_runner(invocation, runner);

        {
            std::lock_guard<std::mutex> lk(state->mu);
            state->active = false;
            if (state->stopping && state->queue.empty()) {
                state->done = true;
                state->done_cv.notify_all();
                return;
            }
        }
    }
}

void HookManager::run_invocation_with_runner(const Invocation& invocation,
                                             const HookProcessRunner& runner) {
    std::string payload_text = invocation.payload.dump();
    LOG_INFO("[hooks] start id=" + invocation.hook.id +
             " event=" + invocation.hook.event +
             " mode=" + mode_name(invocation.hook.mode) +
             " command=" + invocation.command.command +
             " timeout_ms=" + std::to_string(invocation.hook.timeout_ms));

    HookProcessResult result = runner(
        invocation.command,
        payload_text,
        invocation.hook.timeout_ms,
        invocation.cwd);

    std::string status = result.timed_out ? "timeout" :
        (result.started && result.exit_code == 0 ? "ok" : "error");
    std::string line = "[hooks] finish id=" + invocation.hook.id +
        " event=" + invocation.hook.event +
        " status=" + status +
        " exit=" + std::to_string(result.exit_code) +
        " duration_ms=" + std::to_string(result.duration_ms);
    if (!result.error.empty()) {
        line += " error=" + truncate_utf8_prefix(result.error, 300);
    }
    if (!result.output.empty()) {
        line += " output=" + truncate_utf8_prefix(result.output, 500);
    }
    if (status == "ok") {
        LOG_INFO(line);
    } else {
        LOG_WARN(line);
    }
}

} // namespace acecode
