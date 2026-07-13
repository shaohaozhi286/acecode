#include "session_channel_binder.hpp"

#include "session/session_registry.hpp"
#include "utils/logger.hpp"

#include <cctype>
#include <sstream>
#include <utility>

namespace acecode::rc {

namespace {

std::string trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(start, end - start);
}

bool is_blank(const std::string& s) {
    for (unsigned char c : s) {
        if (!std::isspace(c)) return false;
    }
    return true;
}

} // namespace

// ---------- ChannelBindingState ----------

std::uint64_t ChannelBindingState::bind(std::string session_id) {
    session_id_ = std::move(session_id);
    return ++generation_;
}

void ChannelBindingState::unbind() {
    session_id_.clear();
    ++generation_;
}

bool ChannelBindingState::accepts(const std::string& session_id,
                                  std::uint64_t generation) const {
    return bound() && generation == generation_ && session_id == session_id_;
}

bool should_rebuild_binding(const std::string& bound_session_id, bool session_exists) {
    return !bound_session_id.empty() && session_exists;
}

bool resume_session_with_no_workspace_fallback(SessionClient& client,
                                               const std::string& id,
                                               const std::string& cache_root) {
    if (client.resume_session(id)) return true;
    auto meta = find_no_workspace_session_meta(id, cache_root);
    if (!meta) return false;
    SessionOptions opts;
    opts.no_workspace = true;
    // meta.cwd 是创建时落盘的缓存 cwd;为空让 registry 按 id 重新推导。
    opts.cwd = meta->cwd;
    return client.resume_session(id, opts);
}

// ---------- classify_session_event ----------

OutboundEventAction classify_session_event(SessionEventKind kind,
                                           const nlohmann::json& payload) {
    OutboundEventAction action;
    if (!payload.is_object()) return action;

    if (kind == SessionEventKind::Message) {
        if (payload.value("role", std::string{}) != "assistant") return action;
        if (payload.value("is_tool", false)) return action;
        if (!payload.contains("content") || !payload["content"].is_string()) return action;
        std::string content = payload["content"].get<std::string>();
        if (content.empty() || is_blank(content)) return action;
        action.kind = OutboundEventAction::Kind::AssistantText;
        action.text = std::move(content);
        return action;
    }

    if (kind == SessionEventKind::ToolStart) {
        if (!payload.contains("tool") || !payload["tool"].is_string()) return action;
        action.kind = OutboundEventAction::Kind::ToolCall;
        action.tool_name = payload["tool"].get<std::string>();
        action.args = payload.contains("args") ? payload["args"]
                                               : nlohmann::json::object();
        return action;
    }

    return action;
}

// ---------- KeepaliveDecider ----------

KeepaliveDecider::KeepaliveDecider(int failure_threshold,
                                   std::chrono::milliseconds health_interval)
    : failure_threshold_(failure_threshold > 0 ? failure_threshold : 1),
      health_interval_(health_interval),
      last_activation_(Clock::now()) {}

bool KeepaliveDecider::on_outbound_result(bool ok) {
    if (ok) {
        consecutive_failures_ = 0;
        return false;
    }
    if (++consecutive_failures_ >= failure_threshold_) {
        consecutive_failures_ = 0;  // 触发后清零,避免触发风暴
        return true;
    }
    return false;
}

bool KeepaliveDecider::health_due(Clock::time_point now) const {
    return now >= next_health_due();
}

KeepaliveDecider::Clock::time_point KeepaliveDecider::next_health_due() const {
    return last_activation_ + health_interval_;
}

void KeepaliveDecider::note_reactivated(Clock::time_point now) {
    last_activation_ = now;
}

// ---------- SessionChannelBinder ----------

SessionChannelBinder::SessionChannelBinder(SessionChannelBinderDeps deps)
    : deps_(std::move(deps)),
      decider_(deps_.failure_threshold, deps_.health_interval) {}

SessionChannelBinder::~SessionChannelBinder() {
    shutdown();
}

ChannelPluginHost SessionChannelBinder::make_plugin_host() const {
    return deps_.plugin_runner ? ChannelPluginHost(deps_.plugin_runner)
                               : ChannelPluginHost();
}

std::string SessionChannelBinder::bound_session_id() const {
    std::lock_guard<std::mutex> lk(mu_);
    return binding_.bound_session();
}

SessionChannelBinder::CommandOutcome
SessionChannelBinder::execute_command(const std::string& session_id,
                                      const std::string& args) {
    const std::string sub = trim(args);
    if (sub.empty()) return bind_session(session_id);
    if (sub == "off") return unbind_and_stop();
    if (sub == "show") return {true, status_text()};
    return {false,
            "Unknown subcommand. Usage: /rc [off|show] — bare /rc binds this "
            "session to the default channel."};
}

void SessionChannelBinder::with_config_lock(const std::function<void()>& fn) const {
    if (deps_.with_config_lock) {
        deps_.with_config_lock(fn);
    } else {
        fn();
    }
}

void SessionChannelBinder::rebuild_from_config() {
    std::string bound;
    with_config_lock(
        [&] { bound = deps_.config->remote_control.bound_session_id; });
    if (bound.empty()) return;

    bool exists = deps_.session_active && deps_.session_active(bound);
    if (!exists && deps_.session_resumable) {
        exists = deps_.session_resumable(bound);
    }
    if (!should_rebuild_binding(bound, exists)) {
        LOG_WARN("[remote-control] bound session " + bound +
                 " not found; skipping channel binding rebuild");
        return;
    }

    auto outcome = bind_session(bound);
    if (outcome.ok) {
        LOG_INFO("[remote-control] channel binding rebuilt for session " + bound);
    } else {
        LOG_WARN("[remote-control] channel binding rebuild failed: " + outcome.message);
    }
}

SessionChannelBinder::CommandOutcome
SessionChannelBinder::bind_session(const std::string& session_id) {
    std::lock_guard<std::mutex> op(op_mu_);
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (shut_down_) return {false, "remote control is shutting down"};
    }

    // 共享 config 一次性快照(与 daemon 其它读写方同锁互斥),后续流程只用
    // 本地副本 —— 不再裸引用 cfg_mut,config PUT 并发改写也不会撕裂读取。
    std::string channel_name;
    RemoteControlConfig::ChannelPluginConfig channel_cfg;
    bool channel_found = false;
    std::string cfg_token;
    int cfg_port = 0;
    std::string cfg_outbound_url;
    with_config_lock([&] {
        const auto& rc_cfg = deps_.config->remote_control;
        channel_name = rc_cfg.default_channel;
        auto it = rc_cfg.channels.find(channel_name);
        if (it != rc_cfg.channels.end()) {
            channel_found = true;
            channel_cfg = it->second;
        }
        cfg_token = rc_cfg.token;
        cfg_port = rc_cfg.port;
        cfg_outbound_url = rc_cfg.outbound_url;
    });
    if (channel_name.empty()) {
        return {false,
                "No default channel configured. Set remote_control.default_channel "
                "and remote_control.channels in config.json, then re-run /rc."};
    }
    if (!channel_found) {
        return {false, "Default channel '" + channel_name +
                           "' is not configured under remote_control.channels."};
    }

    std::string error;
    auto manifest =
        load_channel_plugin_manifest(channel_cfg.manifest_path, &error);
    if (!manifest.has_value()) {
        return {false, "Failed to load channel plugin '" + channel_name + "': " + error};
    }

    auto& service = *deps_.service;
    std::string token;
    bool started_now = false;
    if (service.running()) {
        token = service.status().token;
        if (token.empty()) return {false, "remote control token is empty"};
    } else {
        token = cfg_token.empty() ? generate_remote_control_token() : cfg_token;
        RemoteControlOptions opts;
        opts.port = cfg_port;
        opts.token = token;
        opts.outbound_url = cfg_outbound_url;
        opts.session_id = session_id;
        if (!service.start(opts, &error)) {
            return {false, "Failed to start remote control: " + error};
        }
        started_now = true;
    }
    const int port = service.status().port;

    // 换绑:先切状态机(世代 +1),旧订阅哪怕还有在途事件也会被 accepts()
    // 拒掉;随后退订旧会话、订阅新会话。
    std::string old_session;
    SessionClient::SubscriptionId old_sub = 0;
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        old_session = binding_.bound_session();
        old_sub = sub_id_;
        sub_id_ = 0;
        generation = binding_.bind(session_id);
    }
    service.hub().set_session_id(session_id);
    if (old_sub != 0) deps_.client->unsubscribe(old_session, old_sub);

    // 入站(行为③):channel 文本走绑定会话的 SessionClient::send_input,
    // AgentLoop::submit 自带 busy 排队语义 —— 与 Web 输入框提交同一条路径。
    service.hub().set_inbound_submit([this](const std::string& text) {
        std::string sid;
        {
            std::lock_guard<std::mutex> lk(mu_);
            sid = binding_.bound_session();
        }
        if (sid.empty()) return;
        deps_.client->send_input(sid, text);
    });
    // 出站结果观察(行为⑤):连续失败达到阈值 → 唤醒保活线程做幂等再激活。
    service.hub().set_outbound_result_observer([this](bool ok) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!active_channel_) return;
        if (decider_.on_outbound_result(ok)) {
            reactivate_now_ = true;
            cv_.notify_all();
        }
    });

    // 出站(行为④):since_seq=0 只收订阅之后的新事件 —— 不回放激活前历史,
    // 镜像 TUI 出站游标语义。回调先过 accepts()(session + generation),
    // 未绑定会话的事件绝不流入 channel。
    auto listener = [this, session_id, generation](const SessionEvent& evt) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!binding_.accepts(session_id, generation)) return;
        auto action = classify_session_event(evt.kind, evt.payload);
        switch (action.kind) {
        case OutboundEventAction::Kind::AssistantText:
            deps_.service->hub().notify_assistant_text(action.text);
            break;
        case OutboundEventAction::Kind::ToolCall:
            deps_.service->hub().notify_tool_call(session_id, action.tool_name,
                                                  action.args);
            break;
        case OutboundEventAction::Kind::None:
            break;
        }
    };
    auto sub = deps_.client->subscribe(session_id, listener, /*since_seq=*/0);
    if (sub == 0) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            binding_.unbind();
            active_channel_.reset();
        }
        service.hub().set_inbound_submit({});
        if (started_now) service.stop();
        return {false, "Session " + session_id + " is not active; cannot bind."};
    }

    ChannelActivationRequest request;
    request.session_id = session_id;
    request.inbound_url = "http://127.0.0.1:" + std::to_string(port) + "/rc/send";
    request.token = token;
    request.settings = channel_cfg.settings.is_object()
                           ? channel_cfg.settings
                           : nlohmann::json::object();
    const int timeout_ms = channel_cfg.timeout_ms > 0
                               ? channel_cfg.timeout_ms
                               : manifest->timeout_ms;
    auto host = make_plugin_host();
    auto activation = host.activate(*manifest, request, timeout_ms, &error);
    if (!activation.ok) {
        deps_.client->unsubscribe(session_id, sub);
        {
            std::lock_guard<std::mutex> lk(mu_);
            binding_.unbind();
            active_channel_.reset();
        }
        service.hub().set_inbound_submit({});
        service.hub().set_outbound_result_observer({});
        if (started_now) service.stop();
        return {false, "Failed to activate channel '" + channel_name + "': " + error};
    }

    service.set_outbound_url(activation.status.outbound_url);
    {
        std::lock_guard<std::mutex> lk(mu_);
        sub_id_ = sub;
        active_channel_ = ActiveChannel{channel_name, *manifest, request,
                                        timeout_ms, generation};
        decider_ = KeepaliveDecider(deps_.failure_threshold, deps_.health_interval);
        decider_.note_reactivated(KeepaliveDecider::Clock::now());
        reactivate_now_ = false;
        cv_.notify_all();
    }

    persist_binding(session_id, token);
    ensure_keepalive_thread();

    std::ostringstream out;
    out << "Channel '" << channel_name << "' connected.";
    if (activation.status.already_running) out << " Existing runtime reused.";
    out << "\nThis session is now bound to the channel";
    if (!old_session.empty() && old_session != session_id) {
        out << " (replaced session " << old_session << ")";
    }
    out << ".\n" << status_text();
    return {true, out.str()};
}

SessionChannelBinder::CommandOutcome SessionChannelBinder::unbind_and_stop() {
    std::lock_guard<std::mutex> op(op_mu_);
    auto& service = *deps_.service;

    std::string old_session;
    SessionClient::SubscriptionId old_sub = 0;
    std::optional<ActiveChannel> active;
    {
        std::lock_guard<std::mutex> lk(mu_);
        old_session = binding_.bound_session();
        old_sub = sub_id_;
        sub_id_ = 0;
        active = active_channel_;
        active_channel_.reset();
        binding_.unbind();
        reactivate_now_ = false;
    }
    service.hub().set_inbound_submit({});
    service.hub().set_outbound_result_observer({});
    if (old_sub != 0) deps_.client->unsubscribe(old_session, old_sub);

    std::string warning;
    if (active.has_value()) {
        std::string error;
        auto host = make_plugin_host();
        if (!host.deactivate(active->manifest, active->request.session_id,
                             active->timeout_ms, &error)) {
            warning = error;
        }
    }

    const bool was_running = service.running();
    service.stop();
    persist_binding("", "");

    std::string message = was_running ? "Remote control stopped."
                                      : "Remote control is not running.";
    if (!warning.empty()) message += "\nChannel deactivate warning: " + warning;
    return {true, message};
}

void SessionChannelBinder::shutdown() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (shut_down_) return;
        shut_down_ = true;
        stop_requested_ = true;
        cv_.notify_all();
    }
    if (keepalive_thread_.joinable()) keepalive_thread_.join();

    // 行为⑥:先停 rc 服务 —— teardown 期间不再接受 channel 入站,也避免
    // 静态析构阶段才停监听的 Crow/asio 顺序问题(镜像 TUI teardown 的顺序,
    // 见 main.cpp shutdown_after_tui_loop)。不 deactivate 插件:与 TUI 退出
    // 一致,channel 运行时保留,bound_session_id 留在 config,下次 daemon
    // 启动走行为①自动重建。
    deps_.service->stop();
    deps_.service->hub().set_inbound_submit({});
    deps_.service->hub().set_outbound_result_observer({});

    std::string old_session;
    SessionClient::SubscriptionId old_sub = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        old_session = binding_.bound_session();
        old_sub = sub_id_;
        sub_id_ = 0;
        binding_.unbind();
        active_channel_.reset();
    }
    if (old_sub != 0) deps_.client->unsubscribe(old_session, old_sub);
}

std::string SessionChannelBinder::status_text() const {
    auto status = deps_.service->status();
    std::string default_channel;
    with_config_lock([&] {
        default_channel = deps_.config->remote_control.default_channel;
    });

    std::ostringstream oss;
    oss << "Remote control : " << (status.running ? "ON" : "OFF");
    if (!default_channel.empty()) {
        oss << "\nDefault channel: " << default_channel;
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (active_channel_.has_value()) {
            oss << "\nActive channel : " << active_channel_->name;
        }
        if (binding_.bound()) {
            oss << "\nBound session  : " << binding_.bound_session();
        }
    }
    if (status.running) {
        oss << "\nInbound        : POST http://127.0.0.1:" << status.port << "/rc/send"
            << "\nToken header   : " << kRemoteControlTokenHeader << ": " << status.token
            << "\nOutbound       : "
            << (status.outbound_url.empty() ? "(not configured)" : status.outbound_url)
            << "\nStats          : in " << status.stats.inbound_accepted << " ok / "
            << status.stats.inbound_rejected << " rejected | out "
            << status.stats.outbound_sent << " sent / " << status.stats.outbound_failed
            << " failed / " << status.stats.outbound_dropped << " dropped";
    } else {
        oss << "\nRun /rc in a session to bind it to the default channel.";
    }
    return oss.str();
}

void SessionChannelBinder::persist_binding(const std::string& bound_session_id,
                                           const std::string& token) {
    // 全程持共享 config 锁:内存更新、磁盘重读、merge、落盘是一个原子步。
    // 落盘不整份序列化内存 config —— 先重读磁盘,只把 binder 拥有的字段
    //(remote_control.bound_session_id / token)merge 进新鲜副本再写回,
    // 避免 stale 内存快照覆盖别的写方(连接器钩子 / config PUT)刚持久化
    // 的字段(如 saved_models 里的 api_key)。
    with_config_lock([&] {
        auto& rc_cfg = deps_.config->remote_control;
        bool dirty = false;
        if (rc_cfg.bound_session_id != bound_session_id) {
            rc_cfg.bound_session_id = bound_session_id;
            dirty = true;
        }
        if (!token.empty() && rc_cfg.token != token) {
            rc_cfg.token = token;
            dirty = true;
        }
        if (!dirty) return;
        AppConfig disk =
            deps_.load_disk_config ? deps_.load_disk_config() : load_config();
        disk.remote_control.bound_session_id = rc_cfg.bound_session_id;
        disk.remote_control.token = rc_cfg.token;
        if (deps_.config_path.empty()) {
            save_config(disk);
        } else {
            save_config(disk, deps_.config_path);
        }
    });
}

void SessionChannelBinder::ensure_keepalive_thread() {
    std::lock_guard<std::mutex> lk(mu_);
    if (keepalive_thread_.joinable() || stop_requested_) return;
    keepalive_thread_ = std::thread([this] { keepalive_loop(); });
}

void SessionChannelBinder::keepalive_loop() {
    std::unique_lock<std::mutex> lk(mu_);
    while (!stop_requested_) {
        if (!active_channel_.has_value()) {
            cv_.wait(lk, [this] {
                return stop_requested_ || active_channel_.has_value();
            });
            continue;
        }
        cv_.wait_until(lk, decider_.next_health_due(), [this] {
            return stop_requested_ || reactivate_now_;
        });
        if (stop_requested_) return;

        bool fire = reactivate_now_;
        reactivate_now_ = false;
        if (!fire && !decider_.health_due(KeepaliveDecider::Clock::now())) continue;
        if (!active_channel_.has_value()) continue;

        lk.unlock();
        {
            // 与 bind/unbind 串行(op_mu_):防止 /rc off 已 deactivate 后,
            // 在途的保活 activate 又把插件拉起来。
            std::lock_guard<std::mutex> op(op_mu_);
            std::optional<ActiveChannel> snapshot;
            {
                std::lock_guard<std::mutex> state(mu_);
                snapshot = active_channel_;
            }
            if (snapshot.has_value()) {
                std::string error;
                auto host = make_plugin_host();
                auto activation = host.activate(snapshot->manifest, snapshot->request,
                                                snapshot->timeout_ms, &error);
                std::lock_guard<std::mutex> state(mu_);
                if (active_channel_.has_value() &&
                    active_channel_->generation == snapshot->generation) {
                    if (activation.ok) {
                        // 幂等再激活成功即视为探活通过;outbound_url 可能已变
                        //(插件重启换端口),热更新出站通道。
                        deps_.service->set_outbound_url(activation.status.outbound_url);
                    } else {
                        LOG_WARN("[remote-control] channel keepalive reactivation "
                                 "failed: " + error);
                    }
                    decider_.note_reactivated(KeepaliveDecider::Clock::now());
                }
            }
        }
        lk.lock();
    }
}

} // namespace acecode::rc
