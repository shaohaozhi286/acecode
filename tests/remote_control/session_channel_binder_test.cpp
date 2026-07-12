// SessionChannelBinder 纯逻辑单测(openspec: daemon 托管 remote control)。
//
// 覆盖三块可测纯逻辑:
//   1. ChannelBindingState —— 绑定状态机:换绑覆盖旧绑定 + generation 过滤
//      (未绑定 session 的事件绝不允许流入 channel,Task 1 评审的硬约束)。
//   2. classify_session_event —— daemon 会话事件 → 出站动作的映射
//      (镜像 TUI 出站游标语义:只转发 assistant 文本与工具调用发起)。
//   3. KeepaliveDecider —— 连续失败阈值 + 60s 周期健康探测的再激活判定。
//
// 集成路径(service 起停 / 插件激活 / registry 订阅)在
// SessionChannelBinderIntegration 分组内用假 plugin runner + 真 registry 验证。

#include <gtest/gtest.h>

#include "remote_control/session_channel_binder.hpp"

#include <chrono>

using namespace std::chrono_literals;
using acecode::SessionEventKind;
using acecode::rc::ChannelBindingState;
using acecode::rc::KeepaliveDecider;
using acecode::rc::OutboundEventAction;
using acecode::rc::classify_session_event;
using acecode::rc::should_rebuild_binding;

// ---------- ChannelBindingState ----------

TEST(ChannelBindingState, StartsUnbound) {
    ChannelBindingState state;
    EXPECT_FALSE(state.bound());
    EXPECT_TRUE(state.bound_session().empty());
    EXPECT_FALSE(state.accepts("any", 0));
    EXPECT_FALSE(state.accepts("any", 1));
}

TEST(ChannelBindingState, BindSetsSessionAndBumpsGeneration) {
    ChannelBindingState state;
    const auto gen = state.bind("ses-a");
    EXPECT_TRUE(state.bound());
    EXPECT_EQ(state.bound_session(), "ses-a");
    EXPECT_EQ(state.generation(), gen);
    EXPECT_TRUE(state.accepts("ses-a", gen));
    // 同 session 但过期 generation(换绑前残留的订阅回调)也必须拒绝。
    EXPECT_FALSE(state.accepts("ses-a", gen + 1));
    EXPECT_FALSE(state.accepts("ses-b", gen));
}

TEST(ChannelBindingState, RebindReplacesOldBinding) {
    ChannelBindingState state;
    const auto gen_a = state.bind("ses-a");
    const auto gen_b = state.bind("ses-b");
    EXPECT_GT(gen_b, gen_a);
    EXPECT_EQ(state.bound_session(), "ses-b");
    // 旧绑定的 (session, generation) 组合全部失效 —— 未绑定会话的事件
    // 不允许泄漏进 channel。
    EXPECT_FALSE(state.accepts("ses-a", gen_a));
    EXPECT_FALSE(state.accepts("ses-a", gen_b));
    EXPECT_FALSE(state.accepts("ses-b", gen_a));
    EXPECT_TRUE(state.accepts("ses-b", gen_b));
}

TEST(ChannelBindingState, UnbindRejectsEverything) {
    ChannelBindingState state;
    const auto gen = state.bind("ses-a");
    state.unbind();
    EXPECT_FALSE(state.bound());
    EXPECT_TRUE(state.bound_session().empty());
    EXPECT_FALSE(state.accepts("ses-a", gen));
    // 重新 bind 后 generation 继续递增,旧代仍然拒绝。
    const auto gen2 = state.bind("ses-a");
    EXPECT_GT(gen2, gen);
    EXPECT_FALSE(state.accepts("ses-a", gen));
    EXPECT_TRUE(state.accepts("ses-a", gen2));
}

// ---------- should_rebuild_binding(启动重建条件,spec §五-3) ----------

TEST(ShouldRebuildBinding, EmptyBoundSessionNeverRebuilds) {
    EXPECT_FALSE(should_rebuild_binding("", true));
    EXPECT_FALSE(should_rebuild_binding("", false));
}

TEST(ShouldRebuildBinding, MissingSessionNeverRebuilds) {
    EXPECT_FALSE(should_rebuild_binding("ses-a", false));
}

TEST(ShouldRebuildBinding, RebuildsWhenBoundSessionExists) {
    EXPECT_TRUE(should_rebuild_binding("ses-a", true));
}

// ---------- classify_session_event ----------

TEST(ClassifySessionEvent, AssistantMessageForwardsText) {
    const auto action = classify_session_event(
        SessionEventKind::Message,
        {{"role", "assistant"}, {"content", "hello from agent"}, {"is_tool", false}});
    EXPECT_EQ(action.kind, OutboundEventAction::Kind::AssistantText);
    EXPECT_EQ(action.text, "hello from agent");
}

TEST(ClassifySessionEvent, IgnoresNonAssistantMessages) {
    const auto user = classify_session_event(
        SessionEventKind::Message,
        {{"role", "user"}, {"content", "hi"}, {"is_tool", false}});
    EXPECT_EQ(user.kind, OutboundEventAction::Kind::None);

    const auto system = classify_session_event(
        SessionEventKind::Message,
        {{"role", "system"}, {"content", "note"}, {"is_tool", false}});
    EXPECT_EQ(system.kind, OutboundEventAction::Kind::None);
}

TEST(ClassifySessionEvent, IgnoresToolAndBlankAssistantMessages) {
    const auto tool = classify_session_event(
        SessionEventKind::Message,
        {{"role", "assistant"}, {"content", "raw tool dump"}, {"is_tool", true}});
    EXPECT_EQ(tool.kind, OutboundEventAction::Kind::None);

    const auto blank = classify_session_event(
        SessionEventKind::Message,
        {{"role", "assistant"}, {"content", "  \n\t "}, {"is_tool", false}});
    EXPECT_EQ(blank.kind, OutboundEventAction::Kind::None);

    const auto missing = classify_session_event(
        SessionEventKind::Message, nlohmann::json{{"role", "assistant"}});
    EXPECT_EQ(missing.kind, OutboundEventAction::Kind::None);
}

TEST(ClassifySessionEvent, ToolStartMapsToToolCall) {
    const auto action = classify_session_event(
        SessionEventKind::ToolStart,
        {{"tool", "bash"},
         {"args", {{"command", "git status"}}},
         {"command_preview", "git status"}});
    EXPECT_EQ(action.kind, OutboundEventAction::Kind::ToolCall);
    EXPECT_EQ(action.tool_name, "bash");
    ASSERT_TRUE(action.args.is_object());
    EXPECT_EQ(action.args["command"], "git status");
}

TEST(ClassifySessionEvent, ToolStartWithoutArgsStillForwards) {
    const auto action = classify_session_event(
        SessionEventKind::ToolStart, nlohmann::json{{"tool", "bash"}});
    EXPECT_EQ(action.kind, OutboundEventAction::Kind::ToolCall);
    EXPECT_EQ(action.tool_name, "bash");

    const auto no_tool = classify_session_event(
        SessionEventKind::ToolStart, nlohmann::json::object());
    EXPECT_EQ(no_tool.kind, OutboundEventAction::Kind::None);
}

TEST(ClassifySessionEvent, OtherEventKindsAreIgnored) {
    for (auto kind : {SessionEventKind::Token, SessionEventKind::Reasoning,
                      SessionEventKind::ToolUpdate, SessionEventKind::ToolEnd,
                      SessionEventKind::BusyChanged, SessionEventKind::Done,
                      SessionEventKind::Usage, SessionEventKind::Error}) {
        const auto action = classify_session_event(
            kind, {{"role", "assistant"}, {"content", "x"}, {"tool", "bash"}});
        EXPECT_EQ(action.kind, OutboundEventAction::Kind::None);
    }
}

// ---------- KeepaliveDecider ----------

TEST(KeepaliveDecider, TriggersAfterConsecutiveFailureThreshold) {
    KeepaliveDecider decider(/*failure_threshold=*/3, /*health_interval=*/60s);
    EXPECT_FALSE(decider.on_outbound_result(false));
    EXPECT_FALSE(decider.on_outbound_result(false));
    EXPECT_TRUE(decider.on_outbound_result(false));
}

TEST(KeepaliveDecider, SuccessResetsFailureStreak) {
    KeepaliveDecider decider(3, 60s);
    EXPECT_FALSE(decider.on_outbound_result(false));
    EXPECT_FALSE(decider.on_outbound_result(false));
    EXPECT_FALSE(decider.on_outbound_result(true));   // 成功清零
    EXPECT_FALSE(decider.on_outbound_result(false));
    EXPECT_FALSE(decider.on_outbound_result(false));
    EXPECT_TRUE(decider.on_outbound_result(false));   // 重新数满 3 次才触发
}

TEST(KeepaliveDecider, TriggerResetsStreakToAvoidStorm) {
    KeepaliveDecider decider(3, 60s);
    decider.on_outbound_result(false);
    decider.on_outbound_result(false);
    EXPECT_TRUE(decider.on_outbound_result(false));
    // 触发后立刻再失败不应连发,要重新数满阈值。
    EXPECT_FALSE(decider.on_outbound_result(false));
    EXPECT_FALSE(decider.on_outbound_result(false));
    EXPECT_TRUE(decider.on_outbound_result(false));
}

TEST(KeepaliveDecider, HealthProbeDueAfterInterval) {
    KeepaliveDecider decider(3, 60s);
    const auto t0 = KeepaliveDecider::Clock::now();
    decider.note_reactivated(t0);
    EXPECT_FALSE(decider.health_due(t0));
    EXPECT_FALSE(decider.health_due(t0 + 59s));
    EXPECT_TRUE(decider.health_due(t0 + 60s));
    EXPECT_TRUE(decider.health_due(t0 + 61s));
    EXPECT_EQ(decider.next_health_due(), t0 + 60s);

    decider.note_reactivated(t0 + 61s);
    EXPECT_FALSE(decider.health_due(t0 + 62s));
    EXPECT_EQ(decider.next_health_due(), t0 + 121s);
}

// ==================== 集成:binder 壳 ====================
//
// 假 plugin runner(不 spawn 进程)+ 真 SessionRegistry/LocalSessionClient +
// 真 RemoteControlService(loopback 测试端口),覆盖行为契约:
//   ② /rc 绑定 + 换绑覆盖 + bound_session_id/token 持久化
//   ③ 入站文本 → 绑定会话的 send_input(AgentLoop 提交路径)
//   ④ 只有绑定会话的 assistant/tool 事件出站(换绑后旧会话事件不得泄漏)
//   ⑤ 出站连续失败 ≥3 → 幂等再激活(runner 收到第二次 channel.activate)
//   ①/⑥ rebuild_from_config 重建 + shutdown 先停服务

#include "remote_control/remote_control_service.hpp"
#include "config/config.hpp"
#include "permissions.hpp"
#include "session/local_session_client.hpp"
#include "session/session_registry.hpp"
#include "tool/tool_executor.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;

int next_binder_port() {
    static std::atomic<int> next{28611};
    return next.fetch_add(7);
}

#ifdef _WIN32
constexpr const char* kBinderHomeEnv = "USERPROFILE";
#else
constexpr const char* kBinderHomeEnv = "HOME";
#endif

class ScopedHome {
public:
    explicit ScopedHome(const fs::path& home) {
        if (const char* cur = std::getenv(kBinderHomeEnv)) {
            had_ = true;
            old_ = cur;
        }
        set(home.string());
    }
    ~ScopedHome() {
        if (had_) set(old_);
        else clear();
    }

private:
    static void set(const std::string& v) {
#ifdef _WIN32
        _putenv_s(kBinderHomeEnv, v.c_str());
#else
        setenv(kBinderHomeEnv, v.c_str(), 1);
#endif
    }
    static void clear() {
#ifdef _WIN32
        _putenv_s(kBinderHomeEnv, "");
#else
        unsetenv(kBinderHomeEnv);
#endif
    }
    bool had_ = false;
    std::string old_;
};

// 记录出站消息的假 sender(与 remote_control_hub_test 同款)。
class CaptureSender : public acecode::rc::OutboundSender {
public:
    bool send(const acecode::rc::OutboundMessage& msg, std::string*) override {
        std::lock_guard<std::mutex> lk(mu_);
        sent_.push_back(msg);
        cv_.notify_all();
        return succeed.load();
    }
    bool wait_for_count(std::size_t n, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(mu_);
        return cv_.wait_for(lk, timeout, [&] { return sent_.size() >= n; });
    }
    std::vector<acecode::rc::OutboundMessage> sent() {
        std::lock_guard<std::mutex> lk(mu_);
        return sent_;
    }
    std::atomic<bool> succeed{true};

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<acecode::rc::OutboundMessage> sent_;
};

// 假 plugin runner 的共享记录:激活/解绑请求 + 计数,支持带超时等待。
struct RunnerLog {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<nlohmann::json> activations;
    std::vector<nlohmann::json> deactivations;

    bool wait_for_activations(std::size_t n, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(mu);
        return cv.wait_for(lk, timeout, [&] { return activations.size() >= n; });
    }
};

acecode::rc::ChannelPluginHost::Runner make_fake_runner(
    std::shared_ptr<RunnerLog> log, std::string outbound_url) {
    return [log = std::move(log), outbound_url = std::move(outbound_url)](
               const acecode::HookCommandSpec&, const std::string& stdin_text,
               int, const std::string&) {
        acecode::HookProcessResult result;
        result.started = true;
        result.exit_code = 0;
        auto request = nlohmann::json::parse(stdin_text, nullptr, false);
        const std::string type =
            request.is_object() ? request.value("type", "") : "";
        {
            std::lock_guard<std::mutex> lk(log->mu);
            if (type == "channel.activate") log->activations.push_back(request);
            if (type == "channel.deactivate") log->deactivations.push_back(request);
            log->cv.notify_all();
        }
        if (type == "channel.activate") {
            nlohmann::json status{
                {"type", "channel.status"},
                {"state", "connected"},
                {"already_running", false},
                {"outbound",
                 nlohmann::json{{"mode", "webhook"}, {"url", outbound_url}}},
            };
            result.stdout_text = status.dump();
        }
        return result;
    };
}

// 最小 daemon 侧脚手架:真 registry + client + service + binder。
struct BinderHarness {
    fs::path root;
    ScopedHome home;
    acecode::ToolExecutor tools;
    acecode::PermissionManager permissions;
    acecode::AppConfig cfg;
    acecode::SessionRegistry registry;
    acecode::LocalSessionClient client;
    acecode::rc::RemoteControlService service;
    std::shared_ptr<RunnerLog> runner_log = std::make_shared<RunnerLog>();
    std::string config_path;

    explicit BinderHarness(const std::string& tag)
        : root(make_root(tag)), home(root / "home"), registry(make_deps(*this)),
          client(registry) {
        cfg.remote_control.port = next_binder_port();
        cfg.remote_control.default_channel = "chat";
        auto manifest_path = root / "channel-plugin.json";
        std::ofstream ofs(manifest_path);
        ofs << nlohmann::json{
            {"schema", "acecode.channel-plugin.v1"},
            {"name", "chat"},
            {"transport", "stdio"},
            {"launcher", nlohmann::json{{"command", "noop-channel-bridge"}}},
        }.dump();
        ofs.close();
        cfg.remote_control.channels["chat"].manifest_path =
            manifest_path.string();
        config_path = (root / "config.json").string();
    }

    ~BinderHarness() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    static fs::path make_root(const std::string& tag) {
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        auto root = fs::temp_directory_path() /
                    ("acecode-binder-" + tag + "-" + std::to_string(now));
        fs::create_directories(root / "home");
        return root;
    }

    static acecode::SessionRegistryDeps make_deps(BinderHarness& self) {
        acecode::SessionRegistryDeps d;
        d.provider_accessor = [] {
            return std::shared_ptr<acecode::LlmProvider>();
        };
        d.tools = &self.tools;
        d.cwd = (self.root / "ws").string();
        d.config = nullptr;
        d.template_permissions = &self.permissions;
        return d;
    }

    acecode::rc::SessionChannelBinderDeps binder_deps(
        const std::string& outbound_url = "http://127.0.0.1:1/hook") {
        acecode::rc::SessionChannelBinderDeps d;
        d.service = &service;
        d.client = &client;
        d.config = &cfg;
        d.config_path = config_path;
        d.session_active = [this](const std::string& id) {
            return registry.acquire(id) != nullptr;
        };
        d.session_resumable = [this](const std::string& id) {
            return client.resume_session(id);
        };
        d.plugin_runner = make_fake_runner(runner_log, outbound_url);
        return d;
    }

    void emit_assistant(const std::string& id, const std::string& text) {
        auto* entry = registry.lookup(id);
        ASSERT_NE(entry, nullptr);
        entry->loop->events().emit(
            acecode::SessionEventKind::Message,
            {{"role", "assistant"}, {"content", text}, {"is_tool", false}});
    }

    void emit_tool_start(const std::string& id, const std::string& tool) {
        auto* entry = registry.lookup(id);
        ASSERT_NE(entry, nullptr);
        entry->loop->events().emit(
            acecode::SessionEventKind::ToolStart,
            {{"tool", tool}, {"args", {{"command", "echo hi"}}}});
    }
};

nlohmann::json read_json_file(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) return nlohmann::json();
    return nlohmann::json::parse(ifs, nullptr, false);
}

} // namespace

TEST(SessionChannelBinderIntegration, BindRebindOffLifecycle) {
    BinderHarness hx("lifecycle");
    acecode::rc::SessionChannelBinder binder(hx.binder_deps());

    const auto s1 = hx.client.create_session({});
    const auto s2 = hx.client.create_session({});

    // ② 绑定 s1:起服务 + 激活默认 channel + 持久化。
    auto bind1 = binder.execute_command(s1, "");
    ASSERT_TRUE(bind1.ok) << bind1.message;
    EXPECT_TRUE(hx.service.running());
    EXPECT_EQ(binder.bound_session_id(), s1);
    EXPECT_EQ(hx.cfg.remote_control.bound_session_id, s1);
    EXPECT_FALSE(hx.cfg.remote_control.token.empty());
    auto persisted = read_json_file(hx.config_path);
    ASSERT_TRUE(persisted.is_object());
    EXPECT_EQ(persisted["remote_control"]["bound_session_id"], s1);
    ASSERT_TRUE(hx.runner_log->wait_for_activations(1, std::chrono::seconds(5)));
    {
        std::lock_guard<std::mutex> lk(hx.runner_log->mu);
        const auto& req = hx.runner_log->activations.at(0);
        EXPECT_EQ(req["session_id"], s1);
        EXPECT_EQ(req["inbound"]["token"], hx.cfg.remote_control.token);
        EXPECT_NE(req["inbound"]["url"].get<std::string>().find("/rc/send"),
                  std::string::npos);
    }

    // ④ 出站:绑定会话的 assistant 文本 / 工具调用出站;先换成捕获 sender
    //(激活返回的 webhook url 不可达,直接投递只会计失败)。
    auto sender1 = std::make_shared<CaptureSender>();
    hx.service.hub().set_outbound_sender(sender1);
    hx.emit_assistant(s1, "bound reply");
    hx.emit_tool_start(s1, "bash");
    ASSERT_TRUE(sender1->wait_for_count(2, std::chrono::seconds(5)));
    {
        auto sent = sender1->sent();
        ASSERT_EQ(sent.size(), 2u);
        EXPECT_EQ(sent[0].type, "assistant_message");
        EXPECT_EQ(sent[0].session_id, s1);
        EXPECT_EQ(sent[0].text, "bound reply");
        EXPECT_EQ(sent[1].type, "tool_call");
        EXPECT_EQ(sent[1].session_id, s1);
        EXPECT_EQ(sent[1].tool_name, "bash");
    }

    // ④ 反向:未绑定会话 s2 的事件绝不出站。
    hx.emit_assistant(s2, "must not leak");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(sender1->sent().size(), 2u);

    // ③ 入站:channel 文本 → 绑定会话的提交路径(AgentLoop 收到该 user 输入)。
    {
        std::mutex mu;
        std::condition_variable cv;
        bool seen = false;
        auto sub = hx.client.subscribe(
            s1,
            [&](const acecode::SessionEvent& evt) {
                if (evt.kind != acecode::SessionEventKind::Message) return;
                if (!evt.payload.is_object()) return;
                if (evt.payload.value("role", "") != "user") return;
                if (evt.payload.value("content", "") != "inbound instruction") return;
                std::lock_guard<std::mutex> lk(mu);
                seen = true;
                cv.notify_all();
            },
            0);
        ASSERT_NE(sub, 0u);
        auto inbound = hx.service.hub().handle_inbound(
            "inbound instruction", hx.cfg.remote_control.token);
        EXPECT_TRUE(inbound.ok()) << inbound.message;
        {
            std::unique_lock<std::mutex> lk(mu);
            EXPECT_TRUE(cv.wait_for(lk, std::chrono::seconds(10),
                                    [&] { return seen; }));
        }
        hx.client.unsubscribe(s1, sub);
    }

    // ② 换绑 s2 覆盖旧绑定 + 持久化更新。
    auto bind2 = binder.execute_command(s2, "");
    ASSERT_TRUE(bind2.ok) << bind2.message;
    EXPECT_EQ(binder.bound_session_id(), s2);
    EXPECT_EQ(hx.cfg.remote_control.bound_session_id, s2);
    EXPECT_EQ(read_json_file(hx.config_path)["remote_control"]["bound_session_id"],
              s2);

    // ④ 换绑后:旧会话 s1 的事件不得泄漏,新会话 s2 正常出站。
    auto sender2 = std::make_shared<CaptureSender>();
    hx.service.hub().set_outbound_sender(sender2);
    hx.emit_assistant(s1, "stale session leak");
    hx.emit_assistant(s2, "rebound reply");
    ASSERT_TRUE(sender2->wait_for_count(1, std::chrono::seconds(5)));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    {
        auto sent = sender2->sent();
        ASSERT_EQ(sent.size(), 1u);
        EXPECT_EQ(sent[0].session_id, s2);
        EXPECT_EQ(sent[0].text, "rebound reply");
    }

    // off:解绑 + 停服务 + 清持久化 + 插件收到 deactivate。
    auto off = binder.execute_command(s2, "off");
    ASSERT_TRUE(off.ok) << off.message;
    EXPECT_FALSE(hx.service.running());
    EXPECT_TRUE(binder.bound_session_id().empty());
    EXPECT_TRUE(hx.cfg.remote_control.bound_session_id.empty());
    {
        std::lock_guard<std::mutex> lk(hx.runner_log->mu);
        ASSERT_GE(hx.runner_log->deactivations.size(), 1u);
        EXPECT_EQ(hx.runner_log->deactivations.back()["session_id"], s2);
    }

    hx.registry.destroy(s1);
    hx.registry.destroy(s2);
}

TEST(SessionChannelBinderIntegration, ConsecutiveOutboundFailuresTriggerReactivation) {
    BinderHarness hx("keepalive");
    auto deps = hx.binder_deps();
    deps.failure_threshold = 3;
    deps.health_interval = std::chrono::milliseconds(60000);  // 周期探测不参与本测
    acecode::rc::SessionChannelBinder binder(std::move(deps));

    const auto s1 = hx.client.create_session({});
    auto bind = binder.execute_command(s1, "");
    ASSERT_TRUE(bind.ok) << bind.message;
    ASSERT_TRUE(hx.runner_log->wait_for_activations(1, std::chrono::seconds(5)));

    // 连续 3 次出站失败 → 保活线程幂等重放 activate(⑤)。
    auto failing = std::make_shared<CaptureSender>();
    failing->succeed = false;
    hx.service.hub().set_outbound_sender(failing);
    hx.emit_assistant(s1, "fail 1");
    hx.emit_assistant(s1, "fail 2");
    hx.emit_assistant(s1, "fail 3");
    EXPECT_TRUE(hx.runner_log->wait_for_activations(2, std::chrono::seconds(10)));

    binder.shutdown();
    EXPECT_FALSE(hx.service.running());
    hx.registry.destroy(s1);
}

TEST(SessionChannelBinderIntegration, RebuildFromConfigRestoresBinding) {
    BinderHarness hx("rebuild");
    const auto s1 = hx.client.create_session({});

    // ① bound_session_id 非空且会话存在 → 自动起服务 + 激活 + 重建绑定。
    hx.cfg.remote_control.bound_session_id = s1;
    {
        acecode::rc::SessionChannelBinder binder(hx.binder_deps());
        binder.rebuild_from_config();
        EXPECT_TRUE(hx.service.running());
        EXPECT_EQ(binder.bound_session_id(), s1);
        ASSERT_TRUE(hx.runner_log->wait_for_activations(1, std::chrono::seconds(5)));
        {
            std::lock_guard<std::mutex> lk(hx.runner_log->mu);
            EXPECT_EQ(hx.runner_log->activations.at(0)["session_id"], s1);
        }
        // ⑥ shutdown 先停服务;绑定持久化保留(下次启动继续重建)。
        binder.shutdown();
        EXPECT_FALSE(hx.service.running());
        EXPECT_EQ(hx.cfg.remote_control.bound_session_id, s1);
    }

    // ① 反向:bound 会话不存在 → 不起服务。
    {
        BinderHarness hx2("rebuild-miss");
        hx2.cfg.remote_control.bound_session_id = "ses-not-there";
        acecode::rc::SessionChannelBinder binder(hx2.binder_deps());
        binder.rebuild_from_config();
        EXPECT_FALSE(hx2.service.running());
        EXPECT_TRUE(binder.bound_session_id().empty());
    }

    hx.registry.destroy(s1);
}
