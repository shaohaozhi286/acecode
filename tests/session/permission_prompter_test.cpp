// 覆盖 src/session/permission_prompter.cpp 的 AsyncPrompter。这是 daemon
// 模式下"工具调用前问浏览器用户"的核心机制。一旦回归:
//   - prompt() 返错决策 → 工具被错误执行/错误拒绝
//   - 超时不触发 deny → worker thread 永远卡住
//   - notify_decision 路由错 → 多并发 prompt 时拿到别人的回复
// 因此每条用例对应一个真实可观测故障。

#include <gtest/gtest.h>

#include "session/event_dispatcher.hpp"
#include "session/permission_prompter.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using acecode::AsyncPrompter;
using acecode::EventDispatcher;
using acecode::PermissionDecisionChoice;
using acecode::PermissionResult;
using acecode::SessionEvent;
using acecode::SessionEventKind;

namespace {

// 注意 race: prompter.prompt() 同步 emit PermissionRequest 后才进 condvar wait。
// 如果 subscribe 在 prompt 之后才注册,会错过那条事件 → 测试卡到超时。
// 因此每条用例都**先**调 subscribe_for_request 装 listener,**再**起线程
// 调 prompter.prompt;listener 里直接 notify_decision。

struct ResponderState {
    std::mutex mu;
    std::string request_id;
    bool got = false;
    std::condition_variable cv;
};

EventDispatcher::SubscriptionId subscribe_responder(
        EventDispatcher& d, AsyncPrompter& prompter,
        PermissionDecisionChoice choice, ResponderState& state) {
    return d.subscribe([&, choice](const SessionEvent& e) {
        if (e.kind != SessionEventKind::PermissionRequest) return;
        {
            std::lock_guard<std::mutex> lk(state.mu);
            state.request_id = e.payload.value("request_id", std::string{});
            state.got = true;
        }
        state.cv.notify_all();
        prompter.notify_decision(state.request_id, choice);
    });
}

struct LifecycleLog {
    std::mutex mu;
    std::vector<SessionEvent> events;

    void record(const SessionEvent& event) {
        if (event.kind != SessionEventKind::PermissionRequest &&
            event.kind != SessionEventKind::PermissionClosed) {
            return;
        }
        std::lock_guard<std::mutex> lk(mu);
        events.push_back(event);
    }

    std::vector<SessionEvent> snapshot() {
        std::lock_guard<std::mutex> lk(mu);
        return events;
    }
};

} // namespace

// 场景: AsyncPrompter::prompt 被 worker thread 调,listener 收到 PermissionRequest
// 后调 notify_decision Allow,prompt 应返回 PermissionResult::Allow。
TEST(AsyncPrompter, AllowDecisionUnblocksPrompt) {
    EventDispatcher d;
    AsyncPrompter prompter(d, 5s);
    ResponderState state;
    LifecycleLog lifecycle;
    auto sub = subscribe_responder(d, prompter, PermissionDecisionChoice::Allow, state);
    auto lifecycle_sub = d.subscribe([&](const SessionEvent& e) { lifecycle.record(e); });

    auto result = prompter.prompt("bash", "{\"command\":\"ls\"}", nullptr);
    EXPECT_EQ(result, PermissionResult::Allow);
    EXPECT_TRUE(state.got);
    EXPECT_FALSE(state.request_id.empty());

    const auto events = lifecycle.snapshot();
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].kind, SessionEventKind::PermissionRequest);
    EXPECT_EQ(events[1].kind, SessionEventKind::PermissionClosed);
    EXPECT_LT(events[0].seq, events[1].seq);
    EXPECT_EQ(events[1].payload.value("request_id", std::string{}), state.request_id);
    EXPECT_EQ(events[1].payload.value("choice", std::string{}), "allow");
    EXPECT_EQ(events[1].payload.value("reason", std::string{}), "decision");

    d.unsubscribe(sub);
    d.unsubscribe(lifecycle_sub);
}

// 场景: Deny 路径必须返回 PermissionResult::Deny。
TEST(AsyncPrompter, DenyDecisionReturnsDeny) {
    EventDispatcher d;
    AsyncPrompter prompter(d, 5s);
    ResponderState state;
    LifecycleLog lifecycle;
    auto sub = subscribe_responder(d, prompter, PermissionDecisionChoice::Deny, state);
    auto lifecycle_sub = d.subscribe([&](const SessionEvent& e) { lifecycle.record(e); });

    auto result = prompter.prompt("bash", "{}", nullptr);
    EXPECT_EQ(result, PermissionResult::Deny);
    const auto events = lifecycle.snapshot();
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[1].payload.value("choice", std::string{}), "deny");
    EXPECT_EQ(events[1].payload.value("reason", std::string{}), "decision");
    d.unsubscribe(sub);
    d.unsubscribe(lifecycle_sub);
}

// 场景: AllowSession 必须映射到 PermissionResult::AlwaysAllow,让 AgentLoop
// 后续调 permissions_.add_session_allow() 记住这次决策。
TEST(AsyncPrompter, AllowSessionMapsToAlwaysAllow) {
    EventDispatcher d;
    AsyncPrompter prompter(d, 5s);
    ResponderState state;
    LifecycleLog lifecycle;
    auto sub = subscribe_responder(d, prompter, PermissionDecisionChoice::AllowSession, state);
    auto lifecycle_sub = d.subscribe([&](const SessionEvent& e) { lifecycle.record(e); });

    auto result = prompter.prompt("bash", "{}", nullptr);
    EXPECT_EQ(result, PermissionResult::AlwaysAllow);
    const auto events = lifecycle.snapshot();
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[1].payload.value("choice", std::string{}), "allow_session");
    EXPECT_EQ(events[1].payload.value("reason", std::string{}), "decision");
    d.unsubscribe(sub);
    d.unsubscribe(lifecycle_sub);
}

// 场景: 超时(spec 规定 5 分钟,这里测试用 200ms)必须返回 Deny + 推 error 事件
// {reason:"permission_timeout"}。这是浏览器关掉/网络断了时不让 worker 卡死的兜底。
TEST(AsyncPrompter, TimeoutReturnsDenyAndEmitsError) {
    EventDispatcher d;
    AsyncPrompter prompter(d, 200ms);

    bool saw_timeout_error = false;
    std::uint64_t timeout_error_seq = 0;
    LifecycleLog lifecycle;
    auto sub = d.subscribe([&](const SessionEvent& e) {
        lifecycle.record(e);
        if (e.kind == SessionEventKind::Error &&
            e.payload.value("reason", std::string{}) == "permission_timeout") {
            saw_timeout_error = true;
            timeout_error_seq = e.seq;
        }
    });

    auto t0 = std::chrono::steady_clock::now();
    auto result = prompter.prompt("bash", "{}", nullptr);
    auto elapsed = std::chrono::steady_clock::now() - t0;

    EXPECT_EQ(result, PermissionResult::Deny);
    EXPECT_GE(elapsed, 200ms) << "至少要等到 timeout 才返回";
    EXPECT_LE(elapsed, 500ms) << "超时返回不应等太久";
    EXPECT_TRUE(saw_timeout_error) << "超时必须 emit error 事件让前端看到";
    const auto events = lifecycle.snapshot();
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[1].kind, SessionEventKind::PermissionClosed);
    EXPECT_EQ(events[1].payload.value("choice", std::string{}), "deny");
    EXPECT_EQ(events[1].payload.value("reason", std::string{}), "timeout");
    EXPECT_LT(events[1].seq, timeout_error_seq)
        << "close 必须先于 timeout error,避免后台客户端先退订而漏掉 close";
    d.unsubscribe(sub);
}

// 场景: abort_flag 触发后 prompter 应当 1s 内返回 Deny(轮询节奏 50ms)。
// 这是用户按 Esc 时不让 worker 卡 5 分钟的关键。
TEST(AsyncPrompter, AbortFlagShortCircuitsToDeny) {
    EventDispatcher d;
    AsyncPrompter prompter(d, 5s);
    LifecycleLog lifecycle;
    auto sub = d.subscribe([&](const SessionEvent& e) { lifecycle.record(e); });

    std::atomic<bool> abort_flag{false};
    std::thread aborter([&] {
        std::this_thread::sleep_for(100ms);
        abort_flag.store(true);
    });

    auto t0 = std::chrono::steady_clock::now();
    auto result = prompter.prompt("bash", "{}", &abort_flag);
    auto elapsed = std::chrono::steady_clock::now() - t0;
    aborter.join();

    EXPECT_EQ(result, PermissionResult::Deny);
    EXPECT_LE(elapsed, 500ms) << "abort 后应快速返回(50ms 轮询 + 100ms 触发延迟)";
    const auto events = lifecycle.snapshot();
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[1].payload.value("choice", std::string{}), "deny");
    EXPECT_EQ(events[1].payload.value("reason", std::string{}), "abort");
    d.unsubscribe(sub);
}

// 场景: 未知 request_id 的 notify_decision 必须是 no-op,不能崩溃 / 抛异常。
// 这覆盖客户端发了一个过时 / 错位 decision 的恶意/异常情况。
TEST(AsyncPrompter, NotifyUnknownRequestIdIsNoOp) {
    EventDispatcher d;
    AsyncPrompter prompter(d, 5s);
    LifecycleLog lifecycle;
    auto sub = d.subscribe([&](const SessionEvent& e) { lifecycle.record(e); });
    EXPECT_NO_THROW(prompter.notify_decision("nonexistent-id",
                                              PermissionDecisionChoice::Allow));
    EXPECT_TRUE(lifecycle.snapshot().empty());
    d.unsubscribe(sub);
}

// 场景:客户端重发/多客户端同时提交同一个 request_id 时只接受第一个决策,
// 并且只能产生一条 permission_closed。
TEST(AsyncPrompter, DuplicateDecisionUsesFirstChoiceAndClosesOnce) {
    EventDispatcher d;
    AsyncPrompter prompter(d, 5s);
    LifecycleLog lifecycle;
    auto sub = d.subscribe([&](const SessionEvent& e) {
        lifecycle.record(e);
        if (e.kind != SessionEventKind::PermissionRequest) return;
        const auto request_id = e.payload.value("request_id", std::string{});
        prompter.notify_decision(request_id, PermissionDecisionChoice::Allow);
        prompter.notify_decision(request_id, PermissionDecisionChoice::Deny);
    });

    EXPECT_EQ(prompter.prompt("bash", "{}", nullptr), PermissionResult::Allow);

    const auto events = lifecycle.snapshot();
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[1].kind, SessionEventKind::PermissionClosed);
    EXPECT_EQ(events[1].payload.value("choice", std::string{}), "allow");
    EXPECT_EQ(events[1].payload.value("reason", std::string{}), "decision");
    d.unsubscribe(sub);
}

// 场景(子代理权限冒泡修复):子代理刚起步就要权限时,前端可能晚于
// permission_request emit 才订阅子会话,而 EventDispatcher since=0 订阅不重放
// ring —— 那条挂起的请求会永久丢失,子代理干等到 5 分钟超时被 Deny(用户实测
// 4m57s / [User denied])。修复靠 WS subscribe 时补发未决请求,数据源即
// snapshot_pending_requests()。本用例验证:prompt 阻塞期间快照能返回该请求的
// request_id / tool / args;应答后快照清空(不补发已决请求,避免弹重复/过时框)。
TEST(AsyncPrompter, SnapshotReturnsPendingRequestUntilResolved) {
    EventDispatcher d;
    AsyncPrompter prompter(d, 5s);
    LifecycleLog lifecycle;
    auto lifecycle_sub = d.subscribe([&](const SessionEvent& e) { lifecycle.record(e); });

    ResponderState state;
    auto sub = d.subscribe([&](const SessionEvent& e) {
        if (e.kind != SessionEventKind::PermissionRequest) return;
        std::lock_guard<std::mutex> lk(state.mu);
        state.request_id = e.payload.value("request_id", std::string{});
        state.got = true;
        state.cv.notify_all();
    });

    std::thread worker([&] {
        prompter.prompt("bash", "{\"command\":\"ls -la\"}", nullptr);
    });

    // 等到 prompt 已 emit(请求已进入 pending_ 表)。
    std::string req_id;
    {
        std::unique_lock<std::mutex> lk(state.mu);
        ASSERT_TRUE(state.cv.wait_for(lk, 2s, [&] { return state.got; }));
        req_id = state.request_id;
    }

    auto pending = prompter.snapshot_pending_requests();
    ASSERT_EQ(pending.size(), 1u) << "阻塞中的权限请求必须能被补发采集到";
    EXPECT_EQ(pending[0].value("request_id", std::string{}), req_id);
    EXPECT_EQ(pending[0].value("tool", std::string{}), "bash");
    ASSERT_TRUE(pending[0].contains("args"));
    EXPECT_EQ(pending[0]["args"].value("command", std::string{}), "ls -la")
        << "补发要带原始 tool/args,前端才能渲染出有意义的确认框";

    // 应答后不再挂起 → 快照清空。
    prompter.notify_decision(req_id, PermissionDecisionChoice::Allow);
    worker.join();
    EXPECT_TRUE(prompter.snapshot_pending_requests().empty())
        << "已决请求不应再被补发";
    const auto events = lifecycle.snapshot();
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[1].payload.value("reason", std::string{}), "decision");

    d.unsubscribe(sub);
    d.unsubscribe(lifecycle_sub);
}

// 场景: 用户在权限弹窗已经出现时切换到 Yolo,daemon 会批量 Allow 所有
// pending prompt,worker 不应继续卡在旧确认框上。
TEST(AsyncPrompter, ResolveAllAllowsPendingPrompt) {
    EventDispatcher d;
    AsyncPrompter prompter(d, 5s);
    LifecycleLog lifecycle;
    auto lifecycle_sub = d.subscribe([&](const SessionEvent& e) { lifecycle.record(e); });

    ResponderState state;
    auto sub = d.subscribe([&](const SessionEvent& e) {
        if (e.kind != SessionEventKind::PermissionRequest) return;
        std::lock_guard<std::mutex> lk(state.mu);
        state.request_id = e.payload.value("request_id", std::string{});
        state.got = true;
        state.cv.notify_all();
    });

    std::atomic<PermissionResult> result{PermissionResult::Deny};
    std::thread worker([&] {
        result = prompter.prompt("bash", "{}", nullptr);
    });

    {
        std::unique_lock<std::mutex> lk(state.mu);
        state.cv.wait_for(lk, 2s, [&] { return state.got; });
    }
    ASSERT_TRUE(state.got);

    prompter.resolve_all(PermissionDecisionChoice::Allow);

    worker.join();
    EXPECT_EQ(result.load(), PermissionResult::Allow);
    const auto events = lifecycle.snapshot();
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[1].payload.value("choice", std::string{}), "allow");
    EXPECT_EQ(events[1].payload.value("reason", std::string{}),
              "permission_mode_change");
    d.unsubscribe(sub);
    d.unsubscribe(lifecycle_sub);
}
