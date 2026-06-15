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

HookProcessResult default_shell_runner(const std::string& command,
                                       const std::string& stdin_text,
                                       int timeout_ms,
                                       const std::string& cwd) {
    return run_hook_shell_command(command, stdin_text, timeout_ms, cwd);
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

bool trust_status_invokable(HookTrustStatus status) {
    return status == HookTrustStatus::Trusted ||
           status == HookTrustStatus::ManagedTrusted;
}

std::string command_for_current_platform(const NormalizedHook& hook) {
#ifdef _WIN32
    if (!hook.command.command_windows.empty()) return hook.command.command_windows;
#endif
    return hook.command.command;
}

} // namespace

HookManager::HookManager()
    : HookManager(HookConfig{}, HookProcessRunner{}) {}

HookManager::HookManager(HookConfig config, HookProcessRunner runner)
    : config_(std::move(config)),
      runner_(runner ? std::move(runner) : HookProcessRunner(default_runner)),
      shell_runner_(HookShellRunner(default_shell_runner)),
      async_state_(std::make_shared<AsyncState>()) {}

HookManager::HookManager(HookRegistrySnapshot registry,
                         HookProcessRunner legacy_runner,
                         HookShellRunner shell_runner)
    : config_{},
      registry_(std::move(registry)),
      runner_(legacy_runner ? std::move(legacy_runner) : HookProcessRunner(default_runner)),
      shell_runner_(shell_runner ? std::move(shell_runner) : HookShellRunner(default_shell_runner)),
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

HookAggregateOutcome HookManager::dispatch_codex(const HookDispatchRequest& request) {
    HookAggregateOutcome aggregate;
    HookRegistrySnapshot registry;
    {
        std::lock_guard<std::mutex> lk(registry_mu_);
        registry = registry_;
    }
    if (!registry.feature_enabled) return aggregate;

    std::vector<HookDiagnostic> matcher_diagnostics;
    const std::string payload_text =
        request.payload.is_object() ? request.payload.dump() : nlohmann::json::object().dump();

    for (const auto& hook : registry.hooks) {
        if (!hook_matcher_matches(hook,
                                  request.event_name,
                                  request.matcher_value,
                                  &matcher_diagnostics)) {
            continue;
        }
        ++aggregate.matched_count;

        if (hook.legacy_direct ||
            hook.kind != HookHandlerKind::Command ||
            hook.skipped ||
            !trust_status_invokable(hook.trust_status)) {
            ++aggregate.skipped_count;
            continue;
        }

        const std::string command = command_for_current_platform(hook);
        if (command.empty()) {
            ++aggregate.skipped_count;
            continue;
        }

        ++aggregate.invoked_count;
        const int timeout_ms = hook.command.timeout_seconds > 0
            ? hook.command.timeout_seconds * 1000
            : 600000;

        LOG_INFO("[hooks] codex start id=" + hook.id +
                 " event=" + request.event_name +
                 " command=" + log_truncate(command, 300) +
                 " timeout_ms=" + std::to_string(timeout_ms));
        HookProcessResult result = shell_runner_(
            command,
            payload_text,
            timeout_ms,
            request.cwd);

        const std::string status = result.timed_out ? "timeout" :
            (result.started && result.exit_code == 0 ? "ok" :
             (result.exit_code == 2 ? "blocked" : "error"));
        std::string line = "[hooks] codex finish id=" + hook.id +
            " event=" + request.event_name +
            " status=" + status +
            " exit=" + std::to_string(result.exit_code) +
            " duration_ms=" + std::to_string(result.duration_ms);
        if (!result.error.empty()) {
            line += " error=" + truncate_utf8_prefix(result.error, 300);
        }
        if (!result.stdout_text.empty()) {
            line += " stdout=" + truncate_utf8_prefix(result.stdout_text, 500);
        }
        if (!result.stderr_text.empty()) {
            line += " stderr=" + truncate_utf8_prefix(result.stderr_text, 500);
        }
        if (status == "ok") LOG_INFO(line);
        else LOG_WARN(line);

        HookParsedOutput output = parse_hook_process_output(result, request.event_name);
        merge_hook_output(aggregate, output, request.event_name, hook);
    }

    aggregate.diagnostics.insert(aggregate.diagnostics.end(),
                                 matcher_diagnostics.begin(),
                                 matcher_diagnostics.end());
    if (aggregate.denied || aggregate.blocked || aggregate.allowed ||
        aggregate.continue_false || aggregate.updated_input.has_value() ||
        aggregate.replacement_output.has_value() ||
        !aggregate.additional_context.empty()) {
        aggregate.no_decision = false;
    }
    return aggregate;
}

HookRegistrySnapshot HookManager::registry_snapshot() const {
    std::lock_guard<std::mutex> lk(registry_mu_);
    return registry_;
}

void HookManager::refresh_registry(HookRegistrySnapshot registry) {
    std::lock_guard<std::mutex> lk(registry_mu_);
    registry_ = std::move(registry);
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
