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

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

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

// 需求④:普通工具调用一律抑制(不出站)。
TEST(ClassifySessionEvent, RegularToolStartSuppressed) {
    const auto action = classify_session_event(
        SessionEventKind::ToolStart,
        {{"tool", "bash"},
         {"args", {{"command", "git status"}}},
         {"command_preview", "git status"},
         {"is_task_complete", false}});
    EXPECT_EQ(action.kind, OutboundEventAction::Kind::None);

    // 缺 is_task_complete 字段的普通工具同样抑制。
    const auto no_flag = classify_session_event(
        SessionEventKind::ToolStart, nlohmann::json{{"tool", "bash"}});
    EXPECT_EQ(no_flag.kind, OutboundEventAction::Kind::None);

    const auto empty = classify_session_event(
        SessionEventKind::ToolStart, nlohmann::json::object());
    EXPECT_EQ(empty.kind, OutboundEventAction::Kind::None);
}

// 需求④:task_complete → 输出 args.summary 全文(作为 AssistantText)。
TEST(ClassifySessionEvent, TaskCompleteForwardsSummaryFullText) {
    const std::string summary = "已完成:改了 3 个文件并跑通全部测试。";
    // is_task_complete 布尔命中。
    const auto by_flag = classify_session_event(
        SessionEventKind::ToolStart,
        {{"tool", "task_complete"},
         {"is_task_complete", true},
         {"args", {{"summary", summary}}}});
    EXPECT_EQ(by_flag.kind, OutboundEventAction::Kind::AssistantText);
    EXPECT_EQ(by_flag.text, summary);

    // 仅靠 tool 名命中(无 is_task_complete 字段)也应识别。
    const auto by_name = classify_session_event(
        SessionEventKind::ToolStart,
        {{"tool", "task_complete"}, {"args", {{"summary", summary}}}});
    EXPECT_EQ(by_name.kind, OutboundEventAction::Kind::AssistantText);
    EXPECT_EQ(by_name.text, summary);

    // task_complete 但 summary 缺失/空白 → 不出站。
    const auto no_summary = classify_session_event(
        SessionEventKind::ToolStart,
        {{"tool", "task_complete"}, {"is_task_complete", true}, {"args", nlohmann::json::object()}});
    EXPECT_EQ(no_summary.kind, OutboundEventAction::Kind::None);
}

// 需求①:Error → reason 原样回传,按字符(码点)截断到 300。
TEST(ClassifySessionEvent, ErrorForwardsReasonTruncatedTo300Codepoints) {
    const auto short_err = classify_session_event(
        SessionEventKind::Error, {{"reason", "模型返回 400:鉴权失效"}});
    EXPECT_EQ(short_err.kind, OutboundEventAction::Kind::AssistantText);
    EXPECT_EQ(short_err.text, "模型返回 400:鉴权失效");

    // 350 个中文字符 → 截到 300 字符(码点),不加省略号。
    std::string long_reason;
    for (int i = 0; i < 350; ++i) long_reason += "错";
    const auto truncated = classify_session_event(
        SessionEventKind::Error, {{"reason", long_reason}});
    ASSERT_EQ(truncated.kind, OutboundEventAction::Kind::AssistantText);
    // 每个"错"是 3 字节;300 码点 = 900 字节。
    EXPECT_EQ(truncated.text.size(), 900u);
    std::string expected300;
    for (int i = 0; i < 300; ++i) expected300 += "错";
    EXPECT_EQ(truncated.text, expected300);

    // 恰好 300 不截。
    const auto exact = classify_session_event(
        SessionEventKind::Error, {{"reason", expected300}});
    EXPECT_EQ(exact.text, expected300);

    // 空白 reason 不出站。
    const auto blank = classify_session_event(
        SessionEventKind::Error, {{"reason", "   "}});
    EXPECT_EQ(blank.kind, OutboundEventAction::Kind::None);
}

TEST(ClassifySessionEvent, OtherEventKindsAreIgnored) {
    // Token/Done 由监听器有状态处理(思考中/复位),纯函数一律 None;Error 与
    // ToolStart 有各自出站规则,不在此列。
    for (auto kind : {SessionEventKind::Token, SessionEventKind::Reasoning,
                      SessionEventKind::ToolUpdate, SessionEventKind::ToolEnd,
                      SessionEventKind::BusyChanged, SessionEventKind::Done,
                      SessionEventKind::Usage}) {
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
            // 与 worker.cpp 的生产接线一致:常规 resume + no-workspace 兜底。
            return acecode::rc::resume_session_with_no_workspace_fallback(client, id);
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
            {{"tool", tool}, {"args", {{"command", "echo hi"}}},
             {"is_task_complete", false}});
    }

    void emit_token(const std::string& id, const std::string& text) {
        auto* entry = registry.lookup(id);
        ASSERT_NE(entry, nullptr);
        entry->loop->events().emit(acecode::SessionEventKind::Token,
                                   {{"text", text}});
    }

    void emit_done(const std::string& id) {
        auto* entry = registry.lookup(id);
        ASSERT_NE(entry, nullptr);
        entry->loop->events().emit(acecode::SessionEventKind::Done,
                                   nlohmann::json::object());
    }

    void emit_task_complete(const std::string& id, const std::string& summary) {
        auto* entry = registry.lookup(id);
        ASSERT_NE(entry, nullptr);
        entry->loop->events().emit(
            acecode::SessionEventKind::ToolStart,
            {{"tool", "task_complete"},
             {"is_task_complete", true},
             {"args", {{"summary", summary}}}});
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

    // ④ 出站:绑定会话的 assistant 文本出站,工具调用一律被抑制(需求④)。
    //    先换成捕获 sender(激活返回的 webhook url 不可达)。绑定确认消息
    //    (需求②)可能在换 sender 前已投给不可达 webhook,故按内容断言、不假设
    //    精确条数。
    auto has_text = [](const std::vector<acecode::rc::OutboundMessage>& v,
                       const std::string& t) {
        for (const auto& m : v) if (m.text == t) return true;
        return false;
    };
    auto has_tool_call = [](const std::vector<acecode::rc::OutboundMessage>& v) {
        for (const auto& m : v) if (m.type == "tool_call") return true;
        return false;
    };
    auto wait_until = [](auto pred, std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (pred()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return pred();
    };

    auto sender1 = std::make_shared<CaptureSender>();
    hx.service.hub().set_outbound_sender(sender1);
    hx.emit_assistant(s1, "bound reply");
    hx.emit_tool_start(s1, "bash");  // 抑制:不产生出站
    ASSERT_TRUE(wait_until([&] { return has_text(sender1->sent(), "bound reply"); },
                           std::chrono::seconds(5)));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_FALSE(has_tool_call(sender1->sent()));  // 工具调用被抑制

    // ④ 反向:未绑定会话 s2 的事件绝不出站。
    hx.emit_assistant(s2, "must not leak");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_FALSE(has_text(sender1->sent(), "must not leak"));

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
    ASSERT_TRUE(wait_until([&] { return has_text(sender2->sent(), "rebound reply"); },
                           std::chrono::seconds(5)));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_FALSE(has_text(sender2->sent(), "stale session leak"));

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

// 需求③④:本轮首个 Token → 一次"思考中...",同轮不重复,Done 后新轮再触发;
// task_complete → 输出 summary 全文;普通工具抑制。
TEST(SessionChannelBinderIntegration, ThinkingHintAndTaskCompleteOutbound) {
    BinderHarness hx("thinking");
    acecode::rc::SessionChannelBinder binder(hx.binder_deps());

    const auto s1 = hx.client.create_session({});
    auto bind1 = binder.execute_command(s1, "");
    ASSERT_TRUE(bind1.ok) << bind1.message;

    auto sender = std::make_shared<CaptureSender>();
    hx.service.hub().set_outbound_sender(sender);

    auto texts = [&] {
        std::vector<std::string> out;
        for (const auto& m : sender->sent()) out.push_back(m.text);
        return out;
    };
    auto count_text = [&](const std::string& t) {
        int n = 0;
        for (const auto& m : sender->sent()) if (m.text == t) ++n;
        return n;
    };
    auto wait_until = [](auto pred, std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (pred()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return pred();
    };

    // 第 1 轮:首个 Token → "思考中...";同轮第二个 Token 不重复。
    hx.emit_token(s1, "你");
    ASSERT_TRUE(wait_until([&] { return count_text("思考中...") == 1; },
                           std::chrono::seconds(5)));
    hx.emit_token(s1, "好");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_EQ(count_text("思考中..."), 1);  // 同轮不重复

    // 助手全文正常出站。
    hx.emit_assistant(s1, "你好，我在。");
    ASSERT_TRUE(wait_until([&] {
        auto v = texts();
        return std::find(v.begin(), v.end(), "你好，我在。") != v.end();
    }, std::chrono::seconds(5)));

    // task_complete → summary 全文出站;普通工具抑制。
    hx.emit_tool_start(s1, "bash");  // 抑制
    hx.emit_task_complete(s1, "已完成:回答了问候。");
    ASSERT_TRUE(wait_until([&] {
        auto v = texts();
        return std::find(v.begin(), v.end(), "已完成:回答了问候。") != v.end();
    }, std::chrono::seconds(5)));
    for (const auto& m : sender->sent()) {
        EXPECT_NE(m.type, "tool_call");  // 全程无 tool_call 出站
    }

    // Done 复位 → 下一轮首个 Token 再次"思考中..."。
    hx.emit_done(s1);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    hx.emit_token(s1, "第");
    ASSERT_TRUE(wait_until([&] { return count_text("思考中...") == 2; },
                           std::chrono::seconds(5)));

    binder.execute_command(s1, "off");
    hx.registry.destroy(s1);
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

// 回归:no_workspace 会话在 daemon 重启后的 resumable 探测。
// 触发场景:/rc 绑定的是「不使用工作区」会话,daemon 重启后启动重建按
// bound_session_id 探测该会话能否从磁盘恢复。
// bug 表现(修复前):探测只调 resume_session(id) 默认 SessionOptions,
// with_resolved_workspace 把 cwd 解析成 daemon 自身 cwd,而 no_workspace
// 会话的 meta 落在 cache/no-workspace/<id>/ 对应的项目目录下,永远 miss →
// 日志 "bound session <id> not found; skipping channel binding rebuild",
// 需要手动重跑 /rc 才能恢复绑定。
TEST(SessionChannelBinderIntegration, NoWorkspaceSessionResumableAfterRestart) {
    BinderHarness hx("no-ws-resume");
    acecode::SessionOptions no_ws;
    no_ws.no_workspace = true;
    const auto sid = hx.client.create_session(no_ws);
    ASSERT_FALSE(sid.empty());
    {
        // meta 是首次落盘才写的(lazy);真实场景绑定过 /rc 的会话必然有过
        // 落盘,这里用 ensure_active_session_id 强制写出 initial meta。
        auto* entry = hx.registry.lookup(sid);
        ASSERT_NE(entry, nullptr);
        ASSERT_EQ(entry->sm->ensure_active_session_id(), sid);
    }
    // 模拟 daemon 重启:内存 registry 清空,磁盘数据保留。
    hx.registry.destroy(sid);
    ASSERT_EQ(hx.registry.lookup(sid), nullptr);

    // 常规 resume(默认 SessionOptions)找不到 no_workspace 会话 —— 这条
    // 断言钉住 bug 的直接根因;若未来常规 resume 自己学会兜底,这里会失败,
    // 提醒同步简化 resume_session_with_no_workspace_fallback。
    EXPECT_FALSE(hx.client.resume_session(sid));

    // 兜底探测应命中缓存目录里的 meta 并以 no_workspace 选项恢复会话。
    EXPECT_TRUE(acecode::rc::resume_session_with_no_workspace_fallback(hx.client, sid));
    auto* resumed = hx.registry.lookup(sid);
    ASSERT_NE(resumed, nullptr);
    EXPECT_TRUE(resumed->no_workspace);

    hx.registry.destroy(sid);
}

// 回归:binder 对共享 AppConfig 的读写与 persist 落盘必须走注入的
// with_config_lock(生产接线 = WebServer::Impl::app_config_mu)。修复前
// binder 在 Crow HTTP 线程上裸读写 cfg_mut 并整份 save_config —— 与
// config PUT / 连接器钩子并发即数据竞争 + config.json 交错写坏。
TEST(SessionChannelBinderIntegration, PersistPathRunsUnderInjectedConfigLock) {
    BinderHarness hx("lock-double");
    auto deps = hx.binder_deps();

    struct LockProbe {
        std::atomic<int> uses{0};
        std::atomic<int> depth{0};
        std::atomic<int> reloads{0};
        std::atomic<int> reloads_outside_lock{0};
    };
    auto probe = std::make_shared<LockProbe>();
    deps.with_config_lock = [probe](const std::function<void()>& fn) {
        probe->uses.fetch_add(1);
        probe->depth.fetch_add(1);
        fn();
        probe->depth.fetch_sub(1);
    };
    deps.load_disk_config = [probe] {
        probe->reloads.fetch_add(1);
        if (probe->depth.load() <= 0) probe->reloads_outside_lock.fetch_add(1);
        return acecode::AppConfig{};
    };
    acecode::rc::SessionChannelBinder binder(std::move(deps));

    const auto s1 = hx.client.create_session({});
    auto bind = binder.execute_command(s1, "");
    ASSERT_TRUE(bind.ok) << bind.message;

    // bind 期间的 config 读取 + persist 的 reload-merge-save 都在锁回调内。
    EXPECT_GE(probe->uses.load(), 2);
    EXPECT_GE(probe->reloads.load(), 1);
    EXPECT_EQ(probe->reloads_outside_lock.load(), 0);
    // 落盘的是 merge 后副本:binder 拥有的字段进了磁盘。
    auto persisted = read_json_file(hx.config_path);
    ASSERT_TRUE(persisted.is_object());
    EXPECT_EQ(persisted["remote_control"]["bound_session_id"], s1);

    binder.shutdown();
    hx.registry.destroy(s1);
}

// 回归:persist_binding 不得用 stale 内存快照整份覆盖磁盘。场景:连接器
// 钩子在 bind 与下一次 persist 之间把 api_key 直写 config.json(生产中
// 钩子预算长达数分钟,重叠窗口真实存在);binder 的内存 config 不知情。
// 修复前:第二次 persist 整份序列化内存 config → 磁盘上刚写入的
// saved_models api_key 被抹掉。修复后:persist 先重读磁盘,只 merge
// binder 拥有的 remote_control.bound_session_id / token,再落盘。
TEST(SessionChannelBinderIntegration, PersistBindingDoesNotClobberOtherWritersDiskState) {
    BinderHarness hx("clobber");
    auto deps = hx.binder_deps();
    // 镜像生产接线:落盘走默认路径(scoped HOME 下的 ~/.acecode/config.json),
    // reload 兜底 acecode::load_config() 读的是同一份文件。
    deps.config_path.clear();
    acecode::rc::SessionChannelBinder binder(std::move(deps));

    const auto s1 = hx.client.create_session({});
    const auto s2 = hx.client.create_session({});
    auto bind1 = binder.execute_command(s1, "");
    ASSERT_TRUE(bind1.ok) << bind1.message;

    // 另一写方(连接器钩子)把新 api_key 落盘;只写磁盘,不碰 binder 内存。
    {
        acecode::AppConfig other = acecode::load_config();
        acecode::ModelProfile profile;
        profile.name = "connector-managed";
        profile.provider = "openai";
        profile.base_url = "http://127.0.0.1:9/v1";
        profile.api_key = "fresh-key-from-hook";
        profile.model = "demo";
        other.saved_models.push_back(profile);
        acecode::save_config(other);
    }

    // 换绑触发第二次 persist —— 修复前这里会把磁盘上的 api_key 清掉。
    auto bind2 = binder.execute_command(s2, "");
    ASSERT_TRUE(bind2.ok) << bind2.message;

    const std::string default_path =
        (fs::path(acecode::get_acecode_dir()) / "config.json").string();
    auto disk = read_json_file(default_path);
    ASSERT_TRUE(disk.is_object());
    EXPECT_EQ(disk["remote_control"]["bound_session_id"], s2);
    ASSERT_TRUE(disk.contains("saved_models") && disk["saved_models"].is_array());
    bool preserved = false;
    for (const auto& m : disk["saved_models"]) {
        if (m.value("name", "") == "connector-managed" &&
            m.value("api_key", "") == "fresh-key-from-hook") {
            preserved = true;
        }
    }
    EXPECT_TRUE(preserved)
        << "another writer's on-disk saved_models entry was clobbered by "
           "persist_binding: " << disk.dump(2);

    binder.shutdown();
    hx.registry.destroy(s1);
    hx.registry.destroy(s2);
}

// 回归:rebuild_from_config 对 no_workspace 绑定会话的完整重启重建链路
//(harness 的 session_resumable 与 worker.cpp 生产接线一致)。
// 期望:服务拉起 + 绑定恢复为原会话 + 激活请求携带原会话 id。
// 修复前:session_resumable 返回 false → should_rebuild_binding 不成立 →
// WARN 跳过,服务不启动、绑定为空(配置不脏写,但远程通道静默失联)。
TEST(SessionChannelBinderIntegration, RebuildFromConfigRestoresNoWorkspaceBinding) {
    BinderHarness hx("rebuild-no-ws");
    acecode::SessionOptions no_ws;
    no_ws.no_workspace = true;
    const auto sid = hx.client.create_session(no_ws);
    ASSERT_FALSE(sid.empty());
    {
        auto* entry = hx.registry.lookup(sid);
        ASSERT_NE(entry, nullptr);
        ASSERT_EQ(entry->sm->ensure_active_session_id(), sid);
    }
    hx.cfg.remote_control.bound_session_id = sid;
    // 模拟 daemon 重启:会话只剩磁盘数据,重建全靠 resumable 探测。
    hx.registry.destroy(sid);

    acecode::rc::SessionChannelBinder binder(hx.binder_deps());
    binder.rebuild_from_config();
    EXPECT_TRUE(hx.service.running());
    EXPECT_EQ(binder.bound_session_id(), sid);
    ASSERT_TRUE(hx.runner_log->wait_for_activations(1, std::chrono::seconds(5)));
    {
        std::lock_guard<std::mutex> lk(hx.runner_log->mu);
        EXPECT_EQ(hx.runner_log->activations.at(0)["session_id"], sid);
    }
    binder.shutdown();
    hx.registry.destroy(sid);
}
