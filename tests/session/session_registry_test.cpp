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
#include "session/session_storage.hpp"
#include "tool/tool_executor.hpp"
#include "utils/cwd_hash.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <random>
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
using acecode::SessionStorage;
using acecode::ToolExecutor;
using acecode::compute_cwd_hash;

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

std::filesystem::path temp_cwd(const std::string& hint) {
    auto dir = std::filesystem::temp_directory_path() /
        ("acecode_registry_" + hint + "_" + std::to_string(std::random_device{}()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

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

// 场景: registry 返回给 HTTP/Web 的 id 必须就是 SessionManager 首次落盘使用
// 的 session id。否则 Web 列表会出现 active registry id + disk id 两条记录。
TEST(SessionRegistry, CreatedIdMatchesSessionManagerDiskId) {
    auto cwd = temp_cwd("id_match");
    auto project_dir = SessionStorage::get_project_dir(cwd.string());
    std::filesystem::remove_all(project_dir);

    ToolExecutor tools;
    PermissionManager permissions;
    SessionRegistryDeps deps;
    deps.provider_accessor = [] { return std::shared_ptr<acecode::LlmProvider>{}; };
    deps.tools = &tools;
    deps.cwd = cwd.string();
    deps.template_permissions = &permissions;
    SessionRegistry registry(std::move(deps));

    auto id = registry.create(SessionOptions{});
    auto* entry = registry.lookup(id);
    ASSERT_NE(entry, nullptr);
    ASSERT_NE(entry->sm, nullptr);

    acecode::ChatMessage msg;
    msg.role = "user";
    msg.content = "hello";
    entry->sm->on_message(msg);

    EXPECT_EQ(entry->sm->current_session_id(), id);
    auto sessions = SessionStorage::list_sessions(project_dir);
    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions[0].id, id);

    std::filesystem::remove_all(project_dir);
    std::filesystem::remove_all(cwd);
}

// 场景: 共享 daemon 下 create 可为非 daemon cwd 的 workspace 创建 session,
// SessionManager / AgentLoop / list_active 都必须使用该 workspace cwd。
TEST(SessionRegistry, CreateUsesWorkspaceCwdFromOptions) {
    auto daemon_cwd = temp_cwd("daemon_cwd");
    auto workspace_cwd = temp_cwd("workspace_cwd");
    auto daemon_project_dir = SessionStorage::get_project_dir(daemon_cwd.string());
    auto workspace_project_dir = SessionStorage::get_project_dir(workspace_cwd.string());
    std::filesystem::remove_all(daemon_project_dir);
    std::filesystem::remove_all(workspace_project_dir);

    ToolExecutor tools;
    PermissionManager permissions;
    SessionRegistryDeps deps;
    deps.provider_accessor = [] { return std::shared_ptr<acecode::LlmProvider>{}; };
    deps.tools = &tools;
    deps.cwd = daemon_cwd.string();
    deps.template_permissions = &permissions;
    SessionRegistry registry(std::move(deps));

    SessionOptions opts;
    opts.cwd = workspace_cwd.string();
    opts.workspace_hash = compute_cwd_hash(opts.cwd);
    auto id = registry.create(opts);
    auto* entry = registry.lookup(id);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->cwd, opts.cwd);
    EXPECT_EQ(entry->workspace_hash, opts.workspace_hash);
    ASSERT_NE(entry->loop, nullptr);
    EXPECT_EQ(entry->loop->cwd(), opts.cwd);

    acecode::ChatMessage msg;
    msg.role = "user";
    msg.content = "workspace scoped";
    entry->sm->on_message(msg);

    EXPECT_FALSE(SessionStorage::list_sessions(workspace_project_dir).empty());
    EXPECT_TRUE(SessionStorage::list_sessions(daemon_project_dir).empty());

    auto active = registry.list_active();
    ASSERT_EQ(active.size(), 1u);
    EXPECT_EQ(active[0].cwd, opts.cwd);
    EXPECT_EQ(active[0].workspace_hash, opts.workspace_hash);

    std::filesystem::remove_all(daemon_project_dir);
    std::filesystem::remove_all(workspace_project_dir);
    std::filesystem::remove_all(daemon_cwd);
    std::filesystem::remove_all(workspace_cwd);
}

// 场景: 同一个共享 daemon registry 可以同时持有不同 workspace 的 active
// session,且每个 AgentLoop / SessionManager 都保持各自 cwd。
TEST(SessionRegistry, CreateTwoWorkspaceSessionsKeepSeparateCwds) {
    auto daemon_cwd = temp_cwd("daemon_multi_cwd");
    auto workspace_a = temp_cwd("workspace_a_cwd");
    auto workspace_b = temp_cwd("workspace_b_cwd");
    auto project_a = SessionStorage::get_project_dir(workspace_a.string());
    auto project_b = SessionStorage::get_project_dir(workspace_b.string());
    std::filesystem::remove_all(project_a);
    std::filesystem::remove_all(project_b);

    ToolExecutor tools;
    PermissionManager permissions;
    SessionRegistryDeps deps;
    deps.provider_accessor = [] { return std::shared_ptr<acecode::LlmProvider>{}; };
    deps.tools = &tools;
    deps.cwd = daemon_cwd.string();
    deps.template_permissions = &permissions;
    SessionRegistry registry(std::move(deps));

    SessionOptions a_opts;
    a_opts.cwd = workspace_a.string();
    a_opts.workspace_hash = compute_cwd_hash(a_opts.cwd);
    SessionOptions b_opts;
    b_opts.cwd = workspace_b.string();
    b_opts.workspace_hash = compute_cwd_hash(b_opts.cwd);

    auto a_id = registry.create(a_opts);
    auto b_id = registry.create(b_opts);
    ASSERT_NE(a_id, b_id);

    auto* a = registry.lookup(a_id);
    auto* b = registry.lookup(b_id);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(a->cwd, a_opts.cwd);
    EXPECT_EQ(b->cwd, b_opts.cwd);
    ASSERT_NE(a->loop, nullptr);
    ASSERT_NE(b->loop, nullptr);
    EXPECT_EQ(a->loop->cwd(), a_opts.cwd);
    EXPECT_EQ(b->loop->cwd(), b_opts.cwd);

    auto active = registry.list_active();
    ASSERT_EQ(active.size(), 2u);
    bool saw_a = false;
    bool saw_b = false;
    for (const auto& info : active) {
        if (info.id == a_id) {
            saw_a = true;
            EXPECT_EQ(info.cwd, a_opts.cwd);
            EXPECT_EQ(info.workspace_hash, a_opts.workspace_hash);
        }
        if (info.id == b_id) {
            saw_b = true;
            EXPECT_EQ(info.cwd, b_opts.cwd);
            EXPECT_EQ(info.workspace_hash, b_opts.workspace_hash);
        }
    }
    EXPECT_TRUE(saw_a);
    EXPECT_TRUE(saw_b);

    std::filesystem::remove_all(project_a);
    std::filesystem::remove_all(project_b);
    std::filesystem::remove_all(daemon_cwd);
    std::filesystem::remove_all(workspace_a);
    std::filesystem::remove_all(workspace_b);
}

// 场景: daemon resume inactive disk session 后,registry 里出现同 id entry,
// AgentLoop 的 provider-facing history 也恢复,后续 user_input 能接着上下文聊。
TEST(SessionRegistry, ResumeDiskSessionRestoresLoopHistory) {
    auto cwd = temp_cwd("resume");
    auto project_dir = SessionStorage::get_project_dir(cwd.string());
    std::filesystem::remove_all(project_dir);
    const std::string id = "20260502-010203-abcd";

    {
        acecode::SessionManager sm;
        sm.start_session(cwd.string(), "test-provider", "test-model", id);
        acecode::ChatMessage msg;
        msg.role = "user";
        msg.content = "remember me";
        sm.on_message(msg);
        sm.finalize();
    }

    ToolExecutor tools;
    PermissionManager permissions;
    SessionRegistryDeps deps;
    deps.provider_accessor = [] { return std::shared_ptr<acecode::LlmProvider>{}; };
    deps.tools = &tools;
    deps.cwd = cwd.string();
    deps.template_permissions = &permissions;
    SessionRegistry registry(std::move(deps));

    ASSERT_TRUE(registry.resume(id));
    auto* entry = registry.lookup(id);
    ASSERT_NE(entry, nullptr);
    ASSERT_NE(entry->loop, nullptr);
    ASSERT_EQ(entry->loop->messages().size(), 1u);
    EXPECT_EQ(entry->loop->messages()[0].content, "remember me");

    EXPECT_TRUE(registry.resume(id)) << "同 daemon 同 id 二次 resume 应复用 active entry";
    EXPECT_EQ(registry.size(), 1u);

    std::filesystem::remove_all(project_dir);
    std::filesystem::remove_all(cwd);
}

// 场景: 共享 daemon 从指定 workspace storage 恢复 session,不能从 daemon cwd
// 的 storage 查找。
TEST(SessionRegistry, ResumeUsesWorkspaceCwdFromOptions) {
    auto daemon_cwd = temp_cwd("resume_daemon");
    auto workspace_cwd = temp_cwd("resume_workspace");
    auto daemon_project_dir = SessionStorage::get_project_dir(daemon_cwd.string());
    auto workspace_project_dir = SessionStorage::get_project_dir(workspace_cwd.string());
    std::filesystem::remove_all(daemon_project_dir);
    std::filesystem::remove_all(workspace_project_dir);
    const std::string id = "20260502-111213-beef";

    {
        acecode::SessionManager sm;
        sm.start_session(workspace_cwd.string(), "test-provider", "test-model", id);
        acecode::ChatMessage msg;
        msg.role = "user";
        msg.content = "from workspace";
        sm.on_message(msg);
        sm.finalize();
    }

    ToolExecutor tools;
    PermissionManager permissions;
    SessionRegistryDeps deps;
    deps.provider_accessor = [] { return std::shared_ptr<acecode::LlmProvider>{}; };
    deps.tools = &tools;
    deps.cwd = daemon_cwd.string();
    deps.template_permissions = &permissions;
    SessionRegistry registry(std::move(deps));

    EXPECT_FALSE(registry.resume(id)) << "未传 workspace 时不应从 daemon cwd 找到其它 workspace 的 session";

    SessionOptions opts;
    opts.cwd = workspace_cwd.string();
    opts.workspace_hash = compute_cwd_hash(opts.cwd);
    ASSERT_TRUE(registry.resume(id, opts));
    auto* entry = registry.lookup(id);
    ASSERT_NE(entry, nullptr);
    ASSERT_NE(entry->loop, nullptr);
    ASSERT_EQ(entry->loop->messages().size(), 1u);
    EXPECT_EQ(entry->loop->messages()[0].content, "from workspace");
    EXPECT_EQ(entry->cwd, opts.cwd);

    std::filesystem::remove_all(daemon_project_dir);
    std::filesystem::remove_all(workspace_project_dir);
    std::filesystem::remove_all(daemon_cwd);
    std::filesystem::remove_all(workspace_cwd);
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
