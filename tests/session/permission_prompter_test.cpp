// 覆盖 src/session/permission_prompter.cpp 的 AsyncPrompter。这是 daemon
// 模式下"工具调用前问浏览器用户"的核心机制。一旦回归:
//   - prompt() 返错决策 → 工具被错误执行/错误拒绝
//   - 超时不触发 deny → worker thread 永远卡住
//   - notify_decision 路由错 → 多并发 prompt 时拿到别人的回复
// 因此每条用例对应一个真实可观测故障。

#include <gtest/gtest.h>

#include "session/event_dispatcher.hpp"
#include "session/permission_prompter.hpp"

#include <chrono>
#include <thread>

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

} // namespace

// 场景: AsyncPrompter::prompt 被 worker thread 调,listener 收到 PermissionRequest
// 后调 notify_decision Allow,prompt 应返回 PermissionResult::Allow。
TEST(AsyncPrompter, AllowDecisionUnblocksPrompt) {
    EventDispatcher d;
    AsyncPrompter prompter(d, 5s);
    ResponderState state;
    auto sub = subscribe_responder(d, prompter, PermissionDecisionChoice::Allow, state);

    auto result = prompter.prompt("bash", "{\"command\":\"ls\"}", nullptr);
    EXPECT_EQ(result, PermissionResult::Allow);
    EXPECT_TRUE(state.got);
    EXPECT_FALSE(state.request_id.empty());
    d.unsubscribe(sub);
}

// 场景: Deny 路径必须返回 PermissionResult::Deny。
TEST(AsyncPrompter, DenyDecisionReturnsDeny) {
    EventDispatcher d;
    AsyncPrompter prompter(d, 5s);
    ResponderState state;
    auto sub = subscribe_responder(d, prompter, PermissionDecisionChoice::Deny, state);

    auto result = prompter.prompt("bash", "{}", nullptr);
    EXPECT_EQ(result, PermissionResult::Deny);
    d.unsubscribe(sub);
}

// 场景: AllowSession 必须映射到 PermissionResult::AlwaysAllow,让 AgentLoop
// 后续调 permissions_.add_session_allow() 记住这次决策。
TEST(AsyncPrompter, AllowSessionMapsToAlwaysAllow) {
    EventDispatcher d;
    AsyncPrompter prompter(d, 5s);
    ResponderState state;
    auto sub = subscribe_responder(d, prompter, PermissionDecisionChoice::AllowSession, state);

    auto result = prompter.prompt("bash", "{}", nullptr);
    EXPECT_EQ(result, PermissionResult::AlwaysAllow);
    d.unsubscribe(sub);
}

// 场景: 超时(spec 规定 5 分钟,这里测试用 200ms)必须返回 Deny + 推 error 事件
// {reason:"permission_timeout"}。这是浏览器关掉/网络断了时不让 worker 卡死的兜底。
TEST(AsyncPrompter, TimeoutReturnsDenyAndEmitsError) {
    EventDispatcher d;
    AsyncPrompter prompter(d, 200ms);

    bool saw_timeout_error = false;
    auto sub = d.subscribe([&](const SessionEvent& e) {
        if (e.kind == SessionEventKind::Error &&
            e.payload.value("reason", std::string{}) == "permission_timeout") {
            saw_timeout_error = true;
        }
    });

    auto t0 = std::chrono::steady_clock::now();
    auto result = prompter.prompt("bash", "{}", nullptr);
    auto elapsed = std::chrono::steady_clock::now() - t0;

    EXPECT_EQ(result, PermissionResult::Deny);
    EXPECT_GE(elapsed, 200ms) << "至少要等到 timeout 才返回";
    EXPECT_LE(elapsed, 500ms) << "超时返回不应等太久";
    EXPECT_TRUE(saw_timeout_error) << "超时必须 emit error 事件让前端看到";
    d.unsubscribe(sub);
}

// 场景: abort_flag 触发后 prompter 应当 1s 内返回 Deny(轮询节奏 50ms)。
// 这是用户按 Esc 时不让 worker 卡 5 分钟的关键。
TEST(AsyncPrompter, AbortFlagShortCircuitsToDeny) {
    EventDispatcher d;
    AsyncPrompter prompter(d, 5s);

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
}

// 场景: 未知 request_id 的 notify_decision 必须是 no-op,不能崩溃 / 抛异常。
// 这覆盖客户端发了一个过时 / 错位 decision 的恶意/异常情况。
TEST(AsyncPrompter, NotifyUnknownRequestIdIsNoOp) {
    EventDispatcher d;
    AsyncPrompter prompter(d, 5s);
    EXPECT_NO_THROW(prompter.notify_decision("nonexistent-id",
                                              PermissionDecisionChoice::Allow));
}
