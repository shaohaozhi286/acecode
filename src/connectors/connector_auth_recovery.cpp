#include "connector_auth_recovery.hpp"

#include "../utils/logger.hpp"

namespace acecode {

namespace {
bool starts_with(const std::string& value, const std::string& prefix) {
    return value.rfind(prefix, 0) == 0;
}
} // namespace

ConnectorAuthRecovery::ConnectorAuthRecovery(Options opts) : opts_(std::move(opts)) {}

void ConnectorAuthRecovery::set_on_config_refreshed(std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(refreshed_cb_mu_);
    opts_.on_config_refreshed = std::move(fn);
}

void ConnectorAuthRecovery::notify_config_refreshed() {
    std::function<void()> cb;
    {
        std::lock_guard<std::mutex> lk(refreshed_cb_mu_);
        cb = opts_.on_config_refreshed;
    }
    if (cb) cb();
}

std::optional<std::string> ConnectorAuthRecovery::fresh_key_from_disk(
        const AppConfig& disk,
        const std::string& model_name,
        const std::string& api_key_at_request) const {
    for (const auto& profile : disk.saved_models) {
        if (profile.name != model_name) continue;
        if (profile.api_key.empty()) continue;
        if (profile.api_key == api_key_at_request) continue;
        return profile.api_key;
    }
    return std::nullopt;
}

std::optional<std::string> ConnectorAuthRecovery::recover(
        const std::string& model_name,
        const std::string& base_url,
        const std::string& api_key_at_request) {
    // AgentLoop 在错误处理路径同步调用本方法;load_disk_config / 钩子执行
    // 抛出的异常绝不能穿透到调用方 —— 一律吞掉并按「恢复失败」处理。
    try {
        if (!opts_.load_disk_config || model_name.empty() || base_url.empty()) {
            return std::nullopt;
        }

        // 连接器定义直接从磁盘读:开关切换会立即持久化,这样既拿到最新状态,
        // 又避免与各 owner 的内存 AppConfig 锁产生耦合。
        const AppConfig snapshot = opts_.load_disk_config();
        const ConnectorConfig* match = nullptr;
        for (const auto& connector : snapshot.connectors) {
            if (!connector.enabled) continue;
            if (connector.auth_error_base_url_prefix.empty()) continue;
            if (!starts_with(base_url, connector.auth_error_base_url_prefix)) continue;
            if (!connector.on_auth_error) continue;
            match = &connector;
            break;
        }
        if (!match) return std::nullopt;

        std::shared_ptr<ConnectorState> state;
        {
            std::lock_guard<std::mutex> lk(states_mu_);
            auto& slot = states_[match->id];
            if (!slot) slot = std::make_shared<ConnectorState>();
            state = slot;
        }
        // 单飞:并发 400 的后来者在这里排队,等前一个登录流程结束。
        std::lock_guard<std::mutex> single_flight(state->mu);

        // 排队期间别的请求可能已完成登录 —— 磁盘上有新 key 就直接用,不再拉起。
        if (auto key = fresh_key_from_disk(opts_.load_disk_config(),
                                           model_name,
                                           api_key_at_request)) {
            notify_config_refreshed();
            return key;
        }

        const auto now = std::chrono::steady_clock::now();
        if (state->last_launch.time_since_epoch().count() != 0 &&
            now - state->last_launch < std::chrono::milliseconds(opts_.cooldown_ms)) {
            LOG_WARN("connector auth recovery cooldown active; id=" + match->id);
            return std::nullopt;
        }
        state->last_launch = now;

        HookCommandSpec cmd;
        cmd.command = match->on_auth_error->command;
        cmd.args = match->on_auth_error->args;
        const int timeout_ms = match->on_auth_error->timeout_ms;
        LOG_INFO("connector auth recovery launching on_auth_error hook; id=" + match->id);
        const HookProcessResult result = opts_.hook_runner
            ? opts_.hook_runner(cmd, timeout_ms)
            : run_hook_process(cmd, std::string{}, timeout_ms, std::string{});
        if (!result.started || result.timed_out || result.exit_code != 0) {
            LOG_WARN("connector auth recovery hook failed; id=" + match->id +
                     " started=" + (result.started ? "true" : "false") +
                     " timed_out=" + (result.timed_out ? "true" : "false") +
                     " exit=" + std::to_string(result.exit_code) +
                     " error=" + result.error);
            return std::nullopt;
        }

        auto key = fresh_key_from_disk(opts_.load_disk_config(),
                                       model_name,
                                       api_key_at_request);
        if (key) {
            LOG_INFO("connector auth recovery succeeded; id=" + match->id);
            notify_config_refreshed();
        } else {
            LOG_WARN("connector auth recovery hook exited 0 but no fresh key on disk; id=" +
                     match->id);
        }
        return key;
    } catch (const std::exception& e) {
        LOG_WARN("connector auth recovery threw: " + std::string(e.what()));
        return std::nullopt;
    } catch (...) {
        LOG_WARN("connector auth recovery threw a non-std exception");
        return std::nullopt;
    }
}

} // namespace acecode
