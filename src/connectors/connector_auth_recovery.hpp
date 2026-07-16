#pragma once

#include "../config/config.hpp"
#include "../hooks/hook_config.hpp"
#include "../hooks/hook_runner.hpp"

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace acecode {

// 连接器认证自动恢复:provider 请求收到认证形态错误(HTTP 400/401)时,按
// auth_error_scope.base_url_prefix 匹配已启用连接器,运行其 on_auth_error
// 钩子(一次性外部进程),然后按 saved model name 从磁盘 config.json 精确
// 读取刷新后的 api_key。
// 具体业务信息(可执行路径、base_url 前缀)全部来自 config.json 数据,
// 本模块保持通用。
class ConnectorAuthRecovery {
public:
    using HookRunnerFn = std::function<HookProcessResult(
        const HookCommandSpec& command, int timeout_ms)>;

    struct Options {
        // 读取磁盘配置(连接器 + saved_models)。生产环境传 load_config;
        // 单测注入假数据。
        std::function<AppConfig()> load_disk_config;
        // 执行钩子进程。空 = 用 run_hook_process。单测注入假 runner。
        HookRunnerFn hook_runner;
        // 磁盘 key 刷新后的回调 —— owner 把磁盘 saved_models 合并回内存
        // AppConfig,防止后续整份 save_config 把新 key 抹掉。可为空。
        std::function<void()> on_config_refreshed;
        int cooldown_ms = 60000;
    };

    explicit ConnectorAuthRecovery(Options opts);

    // 尝试恢复。model_name = 失败请求对应的 saved model name;
    // base_url = 失败请求的 provider base_url;api_key_at_request = 发起该请求
    // 时用的 key(用来区分「该模型刚登录过」和「key 仍旧失效」)。
    // 返回新 key = 已恢复,调用方 update_api_key 后重试一次;
    // nullopt = 不适用 / 冷却中 / 恢复失败。
    // 会阻塞到钩子进程退出或超时,只应在 agent loop 工作线程调用。
    std::optional<std::string> recover(const std::string& model_name,
                                       const std::string& base_url,
                                       const std::string& api_key_at_request);

    // owner(如 web server)构造晚于本服务时,后补回调用。
    void set_on_config_refreshed(std::function<void()> fn);

private:
    struct ConnectorState {
        std::mutex mu;  // 单飞:同连接器同时至多一个钩子进程
        std::chrono::steady_clock::time_point last_launch{};
    };

    std::optional<std::string> fresh_key_from_disk(
        const AppConfig& disk,
        const std::string& model_name,
        const std::string& api_key_at_request) const;
    void notify_config_refreshed();

    Options opts_;
    std::mutex refreshed_cb_mu_;
    std::mutex states_mu_;
    std::map<std::string, std::shared_ptr<ConnectorState>> states_;
};

} // namespace acecode
