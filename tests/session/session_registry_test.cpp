// 覆盖 src/session/session_registry.cpp + src/session/local_session_client.cpp。
// 这是 daemon HTTP handler 跟 worker 沟通的接口层(openspec add-web-daemon
// 任务 7.2 + 7.3)。一旦回归:
//   - HTTP /api/sessions POST 返回错误的 id
//   - WebSocket subscribe 收不到事件
//   - permission_request 响应路由不到正确 session
//   - destroy 内存泄漏 / worker thread 不 join
//
// 测试不真跑 LLM(provider_accessor 返回 nullptr,AgentLoop submit 后
// 第一轮就报 "provider unavailable" 退出 — 不影响我们验 registry/client
// 的接口正确性)。

#include <gtest/gtest.h>

#include "permissions.hpp"
#include "session/local_session_client.hpp"
#include "session/session_registry.hpp"
#include "tool/tool_executor.hpp"

#include <atomic>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;
using acecode::AgentLoop;
using acecode::EventDispatcher;
using acecode::LocalSessionClient;
using acecode::PermissionDecision;
using acecode::PermissionDecisionChoice;
using acecode::PermissionManager;
using acecode::SessionEntry;
using acecode::SessionEvent;
using acecode::SessionEventKind;
using acecode::SessionInfo;
using acecode::SessionOptions;
using acecode::SessionRegistry;
using acecode::SessionRegistryDeps;
using acecode::ToolExecutor;

namespace {

// 构造一个最小的 SessionRegistry: provider_accessor 返回 nullptr(任何 LLM
// 调用会立刻退出),tools 是空的 ToolExecutor,permissions 是默认实例。
struct TestFixture {
    ToolExecutor tools;
    PermissionManager permissions;
    SessionRegistry registry;

    TestFixture()
        : registry(make_deps(*this)) {}

    static SessionRegistryDeps make_deps(TestFixture& self) {
        SessionRegistryDeps d;
        d.provider_accessor = [] { return std::shared_ptr<acecode::LlmProvider>{}; };
        d.tools = &self.tools;
        d.cwd = "/tmp/test_registry";
        d.config = nullptr;
        d.template_permissions = &self.permissions;
        return d;
    }
};

} // namespace

// 场景: create_session 生成的 id 是 SessionStorage::generate_session_id 格式
// (YYYYMMDD-HHMMSS-XXXX),且每次都不同。这是 HTTP handler 把 id 写回响应
// 让客户端记忆的根本前提。
TEST(SessionRegistry, CreateGeneratesUniqueIds) {
    TestFixture fx;
    SessionOptions opts;
    auto a = fx.registry.create(opts);
    auto b = fx.registry.create(opts);
    EXPECT_FALSE(a.empty());
    EXPECT_FALSE(b.empty());
    EXPECT_NE(a, b) << "两次 create 必须生成不同 id";
    EXPECT_EQ(fx.registry.size(), 2u);
    fx.registry.destroy(a);
    fx.registry.destroy(b);
    EXPECT_EQ(fx.registry.size(), 0u);
}

// 场景: lookup 不存在的 session 返回 nullptr,不崩溃。HTTP handler 拿到这个
// 应当回 404,不能 segfault。
TEST(SessionRegistry, LookupNonExistentReturnsNull) {
    TestFixture fx;
    EXPECT_EQ(fx.registry.lookup("nope-doesnt-exist"), nullptr);
}

// 场景: destroy 后 lookup 必须返回 nullptr,且不能再被同 id 找到。
TEST(SessionRegistry, DestroyRemovesEntry) {
    TestFixture fx;
    auto id = fx.registry.create(SessionOptions{});
    EXPECT_NE(fx.registry.lookup(id), nullptr);
    fx.registry.destroy(id);
    EXPECT_EQ(fx.registry.lookup(id), nullptr);
}

// 场景: list_active 必须列出所有当前内存活跃 session,destroy 后从列表消失。
TEST(SessionRegistry, ListActiveReflectsCurrentSessions) {
    TestFixture fx;
    auto a = fx.registry.create(SessionOptions{});
    auto b = fx.registry.create(SessionOptions{});

    auto active = fx.registry.list_active();
    EXPECT_EQ(active.size(), 2u);
    bool seen_a = false, seen_b = false;
    for (const auto& s : active) {
        if (s.id == a) { seen_a = true; EXPECT_TRUE(s.active); }
        if (s.id == b) { seen_b = true; EXPECT_TRUE(s.active); }
    }
    EXPECT_TRUE(seen_a && seen_b);

    fx.registry.destroy(a);
    auto after = fx.registry.list_active();
    EXPECT_EQ(after.size(), 1u);
    EXPECT_EQ(after[0].id, b);
    fx.registry.destroy(b);
}

// 场景: LocalSessionClient::subscribe 必须把 listener 真正接到对应 AgentLoop
// 的 EventDispatcher,从 entry->loop->events().emit 出来的事件 listener 收得到。
TEST(LocalSessionClient, SubscribeReceivesEventsFromCorrectAgentLoop) {
    TestFixture fx;
    LocalSessionClient client(fx.registry);
    auto id = client.create_session(SessionOptions{});

    std::atomic<int> received{0};
    auto sub = client.subscribe(id, [&](const SessionEvent& e) {
        if (e.kind == SessionEventKind::Token) received++;
    });
    ASSERT_NE(sub, 0u);

    // 直接通过 registry 拿 entry,模拟 AgentLoop 内部 emit
    SessionEntry* entry = fx.registry.lookup(id);
    ASSERT_NE(entry, nullptr);
    entry->loop->events().emit(SessionEventKind::Token, {{"text", "hi"}});
    entry->loop->events().emit(SessionEventKind::Token, {{"text", "hi2"}});

    EXPECT_EQ(received.load(), 2);

    client.unsubscribe(id, sub);
    entry->loop->events().emit(SessionEventKind::Token, {{"text", "after-unsub"}});
    EXPECT_EQ(received.load(), 2) << "退订后不应再收到事件";

    client.destroy_session(id);
}

// 场景: subscribe 不存在的 session 返回 0,不崩溃。
TEST(LocalSessionClient, SubscribeOnUnknownSessionReturnsZero) {
    TestFixture fx;
    LocalSessionClient client(fx.registry);
    auto sub = client.subscribe("nope", [](const SessionEvent&) {});
    EXPECT_EQ(sub, 0u);
}

// 场景: respond_permission 必须路由到对应 session 的 AsyncPrompter,prompter
// 在 worker thread 上 prompt 阻塞等的 condvar 应被唤醒。完整 e2e 模拟了
// daemon HTTP handler 收到 decision 包后调 client.respond_permission 的真实路径。
TEST(LocalSessionClient, RespondPermissionRoutesToCorrectPrompter) {
    TestFixture fx;
    LocalSessionClient client(fx.registry);
    auto id = client.create_session(SessionOptions{});

    SessionEntry* entry = fx.registry.lookup(id);
    ASSERT_NE(entry, nullptr);
    ASSERT_NE(entry->prompter, nullptr);

    // 装 listener 收 PermissionRequest 拿 request_id
    std::string captured_id;
    std::mutex mu;
    std::condition_variable cv;
    bool got = false;
    auto sub = client.subscribe(id, [&](const SessionEvent& e) {
        if (e.kind != SessionEventKind::PermissionRequest) return;
        std::lock_guard<std::mutex> lk(mu);
        captured_id = e.payload.value("request_id", std::string{});
        got = true;
        cv.notify_all();
    });

    // 起一个线程模拟 worker 调 prompt
    std::atomic<acecode::PermissionResult> result{acecode::PermissionResult::Deny};
    std::thread worker([&] {
        result = entry->prompter->prompt("bash", "{}", nullptr);
    });

    // 等 PermissionRequest 出现
    {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait_for(lk, 2s, [&] { return got; });
    }
    ASSERT_FALSE(captured_id.empty());

    // 通过 client 回响应
    PermissionDecision dec;
    dec.request_id = captured_id;
    dec.choice = PermissionDecisionChoice::Allow;
    client.respond_permission(id, dec);

    worker.join();
    EXPECT_EQ(result.load(), acecode::PermissionResult::Allow);

    client.unsubscribe(id, sub);
    client.destroy_session(id);
}

// 场景: send_input / abort 在未知 session 上必须是 no-op,不能崩溃。
// 这覆盖客户端用过期 / 错位 id 调接口的健壮性场景。
TEST(LocalSessionClient, OperationsOnUnknownSessionAreNoOp) {
    TestFixture fx;
    LocalSessionClient client(fx.registry);
    EXPECT_NO_THROW(client.send_input("nope", "hello"));
    EXPECT_NO_THROW(client.abort("nope"));
    EXPECT_NO_THROW(client.respond_permission("nope",
        PermissionDecision{"req-x", PermissionDecisionChoice::Allow}));
    EXPECT_NO_THROW(client.destroy_session("nope"));
    EXPECT_NO_THROW(client.unsubscribe("nope", 12345u));
}
