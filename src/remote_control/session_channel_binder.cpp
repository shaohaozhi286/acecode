#include "session_channel_binder.hpp"

#include "session/session_registry.hpp"
#include "utils/logger.hpp"

#include <algorithm>
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

SessionChannelBinder::ContextLease::ContextLease(
    std::shared_ptr<BindingContext> context)
    : context_(std::move(context)) {
    if (!context_) return;
    std::lock_guard<std::mutex> lock(context_->mu);
    if (!context_->active) return;
    ++context_->in_flight;
    entered_ = true;
}

SessionChannelBinder::ContextLease::~ContextLease() {
    if (!entered_ || !context_) return;
    std::lock_guard<std::mutex> lock(context_->mu);
    if (context_->in_flight > 0) --context_->in_flight;
    if (context_->in_flight == 0) context_->cv.notify_all();
}

void SessionChannelBinder::deactivate_context(
    const std::shared_ptr<BindingContext>& context) {
    if (!context) return;
    std::unique_lock<std::mutex> lock(context->mu);
    context->active = false;
    context->outbound_ready.store(false);
    context->cv.wait(lock, [&] { return context->in_flight == 0; });
}

void SessionChannelBinder::emit_question_texts(
    const std::shared_ptr<BindingContext>& context,
    RemoteControlHub* hub,
    const ChannelQuestionAction& action) {
    if (!context || !hub) return;
    if (context->outbound_ready.load()) {
        for (const auto& text : action.outbound_texts) {
            hub->notify_assistant_text(text);
        }
    }
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

namespace {

// 按 Unicode 码点(而非字节)截断到 max_cp;不足则原样返回,不加任何后缀
//(需求:错误"原样"回传、"不超过 300 字")。UTF-8 续字节 0b10xxxxxx 不计入
// 码点数,故中文按"字"计。
std::string truncate_codepoints(const std::string& src, std::size_t max_cp) {
    std::size_t cp = 0;
    for (std::size_t i = 0; i < src.size();) {
        const unsigned char b = static_cast<unsigned char>(src[i]);
        if ((b & 0xC0) == 0x80) { ++i; continue; }  // 续字节:不是新码点起点
        if (cp == max_cp) return src.substr(0, i);
        ++cp;
        ++i;
    }
    return src;
}

}  // namespace

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
        // 工具调用一律不出站,唯一例外:task_complete —— 把它的 summary 全文
        // 作为文本回传(不是 "[工具] …" 摘要、不是 JSON 串)。
        const bool is_task_complete =
            payload.value("is_task_complete", false) ||
            payload.value("tool", std::string{}) == "task_complete";
        if (!is_task_complete) return action;  // 其余工具:抑制
        if (!payload.contains("args") || !payload["args"].is_object()) return action;
        const auto& args = payload["args"];
        if (!args.contains("summary") || !args["summary"].is_string()) return action;
        std::string summary = args["summary"].get<std::string>();
        if (summary.empty() || is_blank(summary)) return action;
        action.kind = OutboundEventAction::Kind::AssistantText;
        action.text = std::move(summary);
        return action;
    }

    if (kind == SessionEventKind::Error) {
        // 一轮出错:把 reason 原样回传,按字符(码点)截断到 300。
        if (!payload.contains("reason") || !payload["reason"].is_string()) return action;
        std::string reason = payload["reason"].get<std::string>();
        if (reason.empty() || is_blank(reason)) return action;
        action.kind = OutboundEventAction::Kind::AssistantText;
        action.text = truncate_codepoints(reason, 300);
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

    // 换绑:先切 generation 并撤销旧 context。旧回调只捕获 shared context，
    // 不捕获 binder；deactivate 等待已线性化的入站/事件完成后再退订，既不
    // 跨 session 投递，也不会在 unsubscribe 的在途窗口访问析构后的 this。
    std::string old_session;
    SessionClient::SubscriptionId old_sub = 0;
    std::shared_ptr<BindingContext> old_context;
    std::uint64_t generation = 0;
    // Hub route 是入站 generation 的线性化点：先切断后续接受，再更新 binder
    // 状态。此前已由 handle_inbound 快照接受的 callback 仍按旧 session 完成。
    service.hub().clear_inbound_route();
    {
        std::lock_guard<std::mutex> lk(mu_);
        old_session = binding_.bound_session();
        old_sub = sub_id_;
        sub_id_ = 0;
        old_context = std::move(binding_context_);
        generation = binding_.bind(session_id);
    }
    deactivate_context(old_context);
    if (old_sub != 0) deps_.client->unsubscribe(old_session, old_sub);

    auto context = std::make_shared<BindingContext>();
    context->session_id = session_id;
    context->generation = generation;
    auto* inbound_client = deps_.client;
    auto* hub = &service.hub();

    // 订阅仍使用 since_seq=0，不回放普通会话历史；QuestionRequest 通过下面
    // 的 pending snapshot 单独补齐。listener 与入站 route 只捕获 context、
    // client 和 hub，binder 析构后即使有在途 dispatcher 回调也不会 UAF。
    auto listener = [context, inbound_client, hub](const SessionEvent& evt) {
        ContextLease lease(context);
        if (!lease) return;
        std::lock_guard<std::mutex> action_lock(context->action_mu);

        if (evt.kind == SessionEventKind::QuestionRequest) {
            std::string error;
            auto request = ChannelQuestionBridge::request_from_event(
                evt.payload,
                evt.seq,
                ChannelQuestionBridge::Clock::now(),
                &error);
            ChannelQuestionAction question_action;
            if (request.has_value()) {
                // 实时帧可能在 dispatcher 队列中等待；LocalSessionClient 的
                // pending 快照持有 prompter 创建时的 steady-clock deadline，
                // 不能把 observed_at + timeout 当成新的整批计时起点。支持
                // 快照但已找不到该请求时，说明它已被另一端回答或关闭。
                if (auto pending = inbound_client->snapshot_pending_questions(
                        context->session_id)) {
                    auto exact = std::find_if(
                        pending->begin(),
                        pending->end(),
                        [&](const PendingQuestionRequestSnapshot& candidate) {
                            return candidate.request_id == request->request_id;
                        });
                    if (exact == pending->end()) return;
                    request = *exact;
                }
                question_action =
                    context->questions->add_request(std::move(*request));
            } else {
                question_action.outbound_texts.push_back(
                    "无法展示待回答问题：" + error);
            }
            emit_question_texts(context, hub, question_action);
            return;
        }
        if (evt.kind == SessionEventKind::QuestionClosed) {
            const std::string request_id =
                evt.payload.value("request_id", std::string{});
            if (request_id.empty()) {
                LOG_WARN("[remote-control] ignored QuestionClosed without request id");
                return;
            }
            auto question_action = context->questions->close_request(
                request_id,
                evt.payload.value("reason", std::string{"closed"}));
            emit_question_texts(context, hub, question_action);
            return;
        }

        auto action = classify_session_event(evt.kind, evt.payload);
        switch (action.kind) {
        case OutboundEventAction::Kind::AssistantText:
            if (context->outbound_ready.load()) {
                hub->notify_assistant_text(action.text);
            }
            break;
        case OutboundEventAction::Kind::ToolCall:  // 已不再产出(工具全部抑制)
        case OutboundEventAction::Kind::None:
            break;
        }
    };
    auto sub = deps_.client->subscribe(session_id, listener, /*since_seq=*/0);
    if (sub == 0) {
        deactivate_context(context);
        {
            std::lock_guard<std::mutex> lk(mu_);
            binding_.unbind();
            active_channel_.reset();
        }
        service.hub().clear_inbound_route();
        if (started_now) service.stop();
        return {false, "Session " + session_id + " is not active; cannot bind."};
    }

    // subscribe 先于 snapshot，避免晚绑定漏问；listener 在 outbound_ready=false
    // 时只更新 bridge，不出站。二者按 request id 去重、按 request_order 排序，
    // QuestionClosed tombstone 可压住“关闭早于快照”的竞态。
    if (auto pending = deps_.client->snapshot_pending_questions(session_id)) {
        std::lock_guard<std::mutex> action_lock(context->action_mu);
        (void)context->questions->merge_snapshot(std::move(*pending));
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
        deactivate_context(context);
        deps_.client->unsubscribe(session_id, sub);
        {
            std::lock_guard<std::mutex> lk(mu_);
            binding_.unbind();
            active_channel_.reset();
        }
        service.hub().clear_inbound_route();
        service.hub().set_outbound_result_observer({});
        if (started_now) service.stop();
        return {false, "Failed to activate channel '" + channel_name + "': " + error};
    }

    service.set_outbound_url(activation.status.outbound_url);
    {
        std::lock_guard<std::mutex> lk(mu_);
        sub_id_ = sub;
        binding_context_ = context;
        active_channel_ = ActiveChannel{channel_name, *manifest, request,
                                        timeout_ms, generation};
        decider_ = KeepaliveDecider(deps_.failure_threshold, deps_.health_interval);
        decider_.note_reactivated(KeepaliveDecider::Clock::now());
        reactivate_now_ = false;
        channel_recovery_pending_ = false;
        cv_.notify_all();
    }

    // 把普通输入与 /aq 控制面作为同一原子 Hub route 发布。Hub 已在调用本
    // callback 前排入“思考中...”；这里的文本/下一题继续进入同一个 FIFO。
    {
        std::lock_guard<std::mutex> action_lock(context->action_mu);
        context->outbound_ready.store(true);
        auto current = context->questions->announce_current();
        emit_question_texts(context, hub, current);
    }
    service.hub().set_inbound_route(
        session_id,
        [context, inbound_client, hub](const std::string& text) {
            ContextLease lease(context);
            if (!lease) return;

            ChannelQuestionAction action;
            {
                std::lock_guard<std::mutex> action_lock(context->action_mu);
                action = context->questions->handle_input(text);
                if (action.handled) {
                    emit_question_texts(context, hub, action);
                }
            }
            if (!action.handled) {
                inbound_client->send_input(context->session_id, text);
                return;
            }
            if (!action.submission.has_value()) return;

            // respond_question 可能唤醒 prompter 并同步/并发回发
            // QuestionClosed；此处不持 binder/context action 锁。
            const auto request_id = action.submission->request_id;
            const auto status = inbound_client->respond_question(
                context->session_id,
                request_id,
                action.submission->response);
            std::lock_guard<std::mutex> action_lock(context->action_mu);
            auto completion = context->questions->complete_submission(
                request_id, status);
            emit_question_texts(context, hub, completion);
        });
    // 出站结果观察(行为⑤):连续失败达到阈值 → 唤醒保活线程做幂等再激活。
    service.hub().set_outbound_result_observer([this](bool ok) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!active_channel_) return;
        if (decider_.on_outbound_result(ok)) {
            reactivate_now_ = true;
            channel_recovery_pending_ = true;
            cv_.notify_all();
        }
    });

    persist_binding(session_id, token);
    ensure_keepalive_thread();

    // 需求②:绑定成功(channel 已激活、出站已就绪)后,主动向 IM 发一条连接
    // 确认。会话名取 title,为空则回退 session id。文本走 assistant 出站路径,
    // channel 插件侧自动加 [ACE] 前缀 —— 此处不重复加。
    {
        std::string title;
        for (const auto& info : deps_.client->list_sessions()) {
            if (info.id == session_id) {
                title = info.title;
                break;
            }
        }
        if (title.empty()) title = session_id;
        service.hub().notify_assistant_text("成功发起远程连接，会话名：" + title);
    }

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
    std::shared_ptr<BindingContext> old_context;
    service.hub().clear_inbound_route();
    {
        std::lock_guard<std::mutex> lk(mu_);
        old_session = binding_.bound_session();
        old_sub = sub_id_;
        sub_id_ = 0;
        old_context = std::move(binding_context_);
        active = active_channel_;
        active_channel_.reset();
        binding_.unbind();
        reactivate_now_ = false;
        channel_recovery_pending_ = false;
    }
    deactivate_context(old_context);
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
    deps_.service->hub().clear_inbound_route();
    deps_.service->stop();
    deps_.service->hub().set_outbound_result_observer({});

    std::string old_session;
    SessionClient::SubscriptionId old_sub = 0;
    std::shared_ptr<BindingContext> old_context;
    {
        std::lock_guard<std::mutex> lk(mu_);
        old_session = binding_.bound_session();
        old_sub = sub_id_;
        sub_id_ = 0;
        old_context = std::move(binding_context_);
        binding_.unbind();
        active_channel_.reset();
        channel_recovery_pending_ = false;
    }
    deactivate_context(old_context);
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
                const auto previous_outbound_url =
                    deps_.service->status().outbound_url;
                auto activation = host.activate(snapshot->manifest, snapshot->request,
                                                snapshot->timeout_ms, &error);
                std::shared_ptr<BindingContext> announce_context;
                {
                    std::lock_guard<std::mutex> state(mu_);
                    if (active_channel_.has_value() &&
                        active_channel_->generation == snapshot->generation) {
                        if (activation.ok) {
                            // 幂等再激活成功即视为探活通过;outbound_url 可能已变
                            //(插件重启换端口),热更新出站通道。只有真实恢复或
                            // 地址变化才重发当前问题，避免周期探活每分钟刷屏。
                            deps_.service->set_outbound_url(
                                activation.status.outbound_url);
                            const bool recovered =
                                fire ||
                                channel_recovery_pending_ ||
                                previous_outbound_url !=
                                    activation.status.outbound_url;
                            channel_recovery_pending_ = false;
                            if (recovered) {
                                announce_context = binding_context_;
                            }
                        } else {
                            channel_recovery_pending_ = true;
                            LOG_WARN(
                                "[remote-control] channel keepalive reactivation "
                                "failed: " + error);
                        }
                        decider_.note_reactivated(
                            KeepaliveDecider::Clock::now());
                    }
                }
                if (announce_context) {
                    ContextLease lease(announce_context);
                    if (lease) {
                        std::lock_guard<std::mutex> action_lock(
                            announce_context->action_mu);
                        auto current =
                            announce_context->questions->announce_current();
                        emit_question_texts(
                            announce_context, &deps_.service->hub(), current);
                    }
                }
            }
        }
        lk.lock();
    }
}

} // namespace acecode::rc
