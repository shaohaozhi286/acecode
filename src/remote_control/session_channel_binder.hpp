#pragma once

// SessionChannelBinder:daemon 托管 remote control 的会话绑定层。
//
// TUI 模式下 /rc 把"当前 TUI 会话"接给 channel(main.cpp 的出站游标 +
// 输入框提交路径);daemon 有多个并存会话,本类把其中**一个**绑定给默认
// channel 插件,并保证:
//   ① worker 启动时若 remote_control.bound_session_id 非空且该会话存在 →
//      自动 start 服务 + 激活默认 channel + 重建绑定(rebuild_from_config)。
//   ② Web 会话执行 /rc → start 服务(未启)+ 激活默认 channel(幂等)+
//      绑定该会话(换绑覆盖旧绑定)+ 持久化 bound_session_id(execute_command)。
//   ③ 入站文本 → 绑定会话的 SessionClient::send_input(AgentLoop::submit
//      自带 busy 排队语义,与 Web 输入框提交同一条路径)。
//   ④ 绑定会话的 assistant 回合文本 → hub notify_assistant_text,工具调用
//      发起 → hub notify_tool_call。订阅回调先经 ChannelBindingState::accepts
//      过滤(session + generation 双重校验)—— 未绑定会话的事件绝不流入
//      channel;订阅用 since_seq=0(只收订阅后新事件),镜像 TUI 出站游标
//      "不回放激活前历史"的语义。
//   ⑤ 出站连续失败 ≥ 阈值(默认 3)或 60s 周期健康探测 → 幂等重放插件
//      activate(插件协议要求 activate 幂等,重放即探活 + 自愈)。
//   ⑥ shutdown() 先 stop 服务(teardown 期间不再接受 channel 入站,也避免
//      静态析构阶段才停监听的顺序问题,镜像 TUI teardown 的注释理由)。
//
// 纯逻辑(ChannelBindingState / classify_session_event / KeepaliveDecider /
// should_rebuild_binding)拆出可单测;网络与插件进程交互留在 binder 壳里,
// 由集成测试(假 plugin runner + 真 SessionRegistry)与 e2e 覆盖。

#include "channel_plugin.hpp"
#include "remote_control_service.hpp"

#include "../config/config.hpp"
#include "../session/session_client.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace acecode::rc {

// ---- 纯逻辑:绑定状态机(换绑覆盖 + 世代过滤) ----
//
// 每次 bind 递增 generation;事件订阅回调持有创建时的 (session_id, generation)
// 快照,投递前用 accepts() 校验 —— 换绑后旧订阅哪怕还有在途事件(unsubscribe
// 与 emit 的竞态窗口),也会因世代不匹配被丢弃。非线程安全,调用方持锁。
class ChannelBindingState {
public:
    // 绑定(换绑覆盖旧绑定)。返回新 generation(从 1 开始单调递增)。
    std::uint64_t bind(std::string session_id);
    void unbind();

    bool bound() const { return !session_id_.empty(); }
    const std::string& bound_session() const { return session_id_; }
    std::uint64_t generation() const { return generation_; }

    // 只有"当前绑定会话 + 当前世代"的事件允许出站。
    bool accepts(const std::string& session_id, std::uint64_t generation) const;

private:
    std::string session_id_;
    std::uint64_t generation_ = 0;
};

// 启动重建条件(spec §五-3):bound_session_id 非空且该会话存在。
bool should_rebuild_binding(const std::string& bound_session_id, bool session_exists);

// worker 给 SessionChannelBinderDeps::session_resumable 的标准接线:常规
// resume(默认 SessionOptions,按 daemon cwd 解析 workspace)失败后,按
// no-workspace 缓存目录兜底查 meta,命中则以 no_workspace 选项重试。
// 不兜底时,绑定 no_workspace 会话的 daemon 重启后启动重建必然失败
//(meta 在 cache/no-workspace/<id>/ 对应项目目录下,常规解析永远 miss,
// 日志报 "bound session not found; skipping channel binding rebuild")。
bool resume_session_with_no_workspace_fallback(SessionClient& client,
                                               const std::string& id,
                                               const std::string& cache_root = {});

// ---- 纯逻辑:daemon 会话事件 → 出站动作 ----
//
// Message(role=assistant 且非工具、非空白)→ AssistantText;ToolStart →
// ToolCall;其余(Token 流式增量 / ToolUpdate / Busy...)一律 None ——
// 流式期间的半截文本不出站,与 TUI "回合结束才转发" 的粒度一致
// (AgentLoop 在文本回合结束 / 工具回合开始时各 emit 一条权威 Message)。
struct OutboundEventAction {
    enum class Kind { None, AssistantText, ToolCall };
    Kind kind = Kind::None;
    std::string text;       // AssistantText
    std::string tool_name;  // ToolCall
    nlohmann::json args;    // ToolCall(缺省空 object)
};

OutboundEventAction classify_session_event(SessionEventKind kind,
                                           const nlohmann::json& payload);

// ---- 纯逻辑:保活判定 ----
//
// on_outbound_result(false) 连续满 failure_threshold 次 → 返回 true(应立即
// 再激活)并清零计数(防触发风暴);成功清零。health_due/next_health_due
// 提供 60s 周期探测的时间判定,note_reactivated 重置周期。非线程安全,
// 调用方持锁。
class KeepaliveDecider {
public:
    using Clock = std::chrono::steady_clock;

    explicit KeepaliveDecider(
        int failure_threshold = 3,
        std::chrono::milliseconds health_interval = std::chrono::milliseconds(60000));

    // 记录一次出站投递结果。返回 true = 连续失败达到阈值,应立即再激活。
    bool on_outbound_result(bool ok);

    bool health_due(Clock::time_point now) const;
    Clock::time_point next_health_due() const;
    void note_reactivated(Clock::time_point now);

    int consecutive_failures() const { return consecutive_failures_; }

private:
    int failure_threshold_;
    std::chrono::milliseconds health_interval_;
    int consecutive_failures_ = 0;
    Clock::time_point last_activation_{};
};

// ---- 集成壳 ----

struct SessionChannelBinderDeps {
    RemoteControlService* service = nullptr;  // 必填(worker 传进程单例)
    SessionClient*        client  = nullptr;  // 必填(订阅事件流 + send_input)
    AppConfig*            config  = nullptr;  // 必填(remote_control 段读写)
    std::string           config_path;        // 空 = save_config 默认路径

    // config 是 daemon 全进程共享的可变对象(HTTP 路由 / 连接器钩子都在
    // 读写);binder 对它的所有读写必须放进这个回调里执行,由锁的 owner
    //(WebServer::Impl::app_config_mu,经 WebServer::with_app_config_lock
    // 注入)持锁调用 —— binder 不自己抓全局锁。空 = 直接执行(纯单测,
    // 无并发写方)。回调内不得再进 binder 自己的 op_mu_/mu_。
    std::function<void(const std::function<void()>&)> with_config_lock;

    // persist 前 reload-merge 的磁盘读取(防 stale 内存快照整份覆盖掉别的
    // 写方刚落盘的字段,如连接器钩子直写的 saved_models api_key)。
    // 空 = acecode::load_config()(生产 config_path 即默认路径,读写同一份)。
    std::function<AppConfig()> load_disk_config;

    // 会话存在性探测:active = 当前在内存 registry 中;resumable = 可从磁盘
    // 恢复进 registry(rebuild 用,恢复成功即视为存在)。
    std::function<bool(const std::string&)> session_active;
    std::function<bool(const std::string&)> session_resumable;

    // 测试注入的插件进程 runner;空 = ChannelPluginHost::default_runner()。
    ChannelPluginHost::Runner plugin_runner;

    int failure_threshold = 3;
    std::chrono::milliseconds health_interval{60000};
};

class SessionChannelBinder {
public:
    struct CommandOutcome {
        bool ok = false;
        std::string message;
    };

    explicit SessionChannelBinder(SessionChannelBinderDeps deps);
    ~SessionChannelBinder();

    SessionChannelBinder(const SessionChannelBinder&) = delete;
    SessionChannelBinder& operator=(const SessionChannelBinder&) = delete;

    // /rc 与 /remote-control 的 daemon 实现:args 空 = 绑定该会话到默认
    // channel;"off" = 解绑 + 停服务;"show" = 状态文本。
    CommandOutcome execute_command(const std::string& session_id,
                                   const std::string& args);

    // 行为①:worker 启动时按持久化的 bound_session_id 重建绑定。失败只
    // 记日志,不影响 daemon 启动。
    void rebuild_from_config();

    // 行为⑥:先停 rc 服务,再退订/清回调。幂等;析构自动调用。
    void shutdown();

    std::string bound_session_id() const;

private:
    struct ActiveChannel {
        std::string name;
        ChannelPluginManifest manifest;
        ChannelActivationRequest request;
        int timeout_ms = 10000;
        std::uint64_t generation = 0;
    };

    CommandOutcome bind_session(const std::string& session_id);
    CommandOutcome unbind_and_stop();
    std::string status_text() const;
    // deps_.config 的读写统一走这里(deps_.with_config_lock 持锁执行;未注入
    // 则直接执行)。锁序:op_mu_ > app_config_mu(本回调)>(不进)mu_。
    void with_config_lock(const std::function<void()>& fn) const;
    void persist_binding(const std::string& bound_session_id,
                         const std::string& token);
    void ensure_keepalive_thread();
    void keepalive_loop();
    ChannelPluginHost make_plugin_host() const;

    SessionChannelBinderDeps deps_;

    // 锁序:op_mu_(慢路径串行化)> mu_(状态)> hub/service 内部锁。
    // 订阅回调 / 出站结果观察者只拿 mu_。
    std::mutex op_mu_;
    mutable std::mutex mu_;
    std::condition_variable cv_;

    ChannelBindingState binding_;
    SessionClient::SubscriptionId sub_id_ = 0;
    std::optional<ActiveChannel> active_channel_;
    KeepaliveDecider decider_;
    bool reactivate_now_ = false;
    bool stop_requested_ = false;
    bool shut_down_ = false;
    std::thread keepalive_thread_;
};

} // namespace acecode::rc
