// 覆盖 src/desktop/daemon_pool.cpp。pool 是 desktop 多 workspace 模型的核心 —
// 同 hash 并发 race / 不同 hash 并发隔离 / stop_all best-effort,任何一项跑偏
// 用户都能在 sidebar 上看到不该有的状态。这里用 mock supervisor 注入,不起
// 真实 acecode.exe 子进程(那是手动 smoke 才覆盖)。

#include <gtest/gtest.h>

#include "desktop/daemon_pool.hpp"
#include "desktop/daemon_supervisor.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

using acecode::desktop::ActivateRequest;
using acecode::desktop::ActivateResult;
using acecode::desktop::DaemonPool;
using acecode::desktop::DaemonState;
using acecode::desktop::IDaemonSupervisor;
using acecode::desktop::SpawnRequest;
using acecode::desktop::SpawnResult;

namespace {

// MockSupervisor: spawn / wait_until_ready 行为可配置;stop 计数。线程安全。
class MockSupervisor : public IDaemonSupervisor {
public:
    struct SharedState {
        std::atomic<int> spawn_calls{0};
        std::atomic<int> wait_calls{0};
        std::atomic<int> stop_calls{0};
        std::atomic<bool> spawn_should_succeed{true};
        std::atomic<bool> ready_should_succeed{true};
        // 模拟 spawn 耗时,放大并发竞争窗口。
        std::chrono::milliseconds spawn_delay{0};
        // wait_until_ready 中的延迟,确保等待者真的会进 cv.wait 分支。
        std::chrono::milliseconds wait_delay{0};
        std::mutex mu;
        std::vector<std::string> run_dirs;
    };
    explicit MockSupervisor(std::shared_ptr<SharedState> s) : state_(std::move(s)) {}

    SpawnResult spawn(const SpawnRequest& req) override {
        state_->spawn_calls.fetch_add(1);
        {
            std::lock_guard<std::mutex> lk(state_->mu);
            state_->run_dirs.push_back(req.run_dir);
        }
        if (state_->spawn_delay.count() > 0) {
            std::this_thread::sleep_for(state_->spawn_delay);
        }
        SpawnResult r;
        if (state_->spawn_should_succeed.load()) {
            r.ok = true;
            r.pid = 4242;
        } else {
            r.error = "mock: spawn_should_succeed=false";
        }
        return r;
    }

    bool wait_until_ready(int, std::chrono::milliseconds) override {
        state_->wait_calls.fetch_add(1);
        if (state_->wait_delay.count() > 0) {
            std::this_thread::sleep_for(state_->wait_delay);
        }
        return state_->ready_should_succeed.load();
    }

    void stop() override { state_->stop_calls.fetch_add(1); }
    bool running() const override { return false; }

private:
    std::shared_ptr<SharedState> state_;
};

// 构造一个标准 ActivateRequest(用 hash="h1" 的常用 default,测试可改)
ActivateRequest make_request(const std::string& hash, const std::string& cwd = "/tmp/x") {
    ActivateRequest r;
    r.hash = hash;
    r.cwd = cwd;
    r.daemon_exe_path = "/fake/path/acecode.exe";
    return r;
}

} // namespace

// 场景: 单次 activate 成功 → 返回 ok + port + token,state 变 Running
TEST(DaemonPool, ActivateOnceSucceeds) {
    auto state = std::make_shared<MockSupervisor::SharedState>();
    DaemonPool pool;
    pool.set_supervisor_factory_for_test([&] {
        return std::unique_ptr<IDaemonSupervisor>(new MockSupervisor(state));
    });
    auto r = pool.activate(make_request("h1"));
    EXPECT_TRUE(r.ok) << r.error;
    EXPECT_GT(r.port, 0);          // pick_free_loopback_port 真的去拿了 port
    EXPECT_FALSE(r.token.empty()); // make_auth_token 真的生成了

    auto snap = pool.lookup("h1");
    EXPECT_EQ(snap.state, DaemonState::Running);
    EXPECT_EQ(state->spawn_calls.load(), 1);
}

// 场景: 同 hash 并发 activate → spawn 只被调一次,所有 caller 拿到相同 port/token
TEST(DaemonPool, SameHashConcurrentSpawnsOnce) {
    auto state = std::make_shared<MockSupervisor::SharedState>();
    state->spawn_delay = std::chrono::milliseconds(100); // 放大竞争窗口
    DaemonPool pool;
    pool.set_supervisor_factory_for_test([&] {
        return std::unique_ptr<IDaemonSupervisor>(new MockSupervisor(state));
    });

    constexpr int kThreads = 6;
    std::vector<std::thread> ts;
    std::vector<ActivateResult> results(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        ts.emplace_back([&, i] {
            results[i] = pool.activate(make_request("h-shared"));
        });
    }
    for (auto& t : ts) t.join();

    // 所有 6 次 activate 都成功
    for (auto& r : results) {
        EXPECT_TRUE(r.ok) << r.error;
    }
    // 但 spawn 只调一次
    EXPECT_EQ(state->spawn_calls.load(), 1);

    // 所有 caller 看到相同 port / token
    int port0 = results[0].port;
    std::string token0 = results[0].token;
    for (auto& r : results) {
        EXPECT_EQ(r.port, port0);
        EXPECT_EQ(r.token, token0);
    }
}

// 场景: 不同 hash 并发 activate 各自独立 spawn(不串行化),工厂被调 N 次
TEST(DaemonPool, DifferentHashesIndependent) {
    auto state = std::make_shared<MockSupervisor::SharedState>();
    DaemonPool pool;
    pool.set_supervisor_factory_for_test([&] {
        return std::unique_ptr<IDaemonSupervisor>(new MockSupervisor(state));
    });

    constexpr int kHashes = 4;
    std::vector<std::thread> ts;
    for (int i = 0; i < kHashes; ++i) {
        ts.emplace_back([&, i] {
            pool.activate(make_request("h" + std::to_string(i)));
        });
    }
    for (auto& t : ts) t.join();

    EXPECT_EQ(state->spawn_calls.load(), kHashes);
    EXPECT_EQ(pool.snapshot_all().size(), static_cast<size_t>(kHashes));
}

// 场景: 同一 workspace hash 的不同 context_id 必须是两个独立 daemon slot。
// Desktop 同 cwd 多 resume 就靠这个把 runtime files / pid / provider 状态隔开。
TEST(DaemonPool, SameHashDifferentContextsSpawnIndependently) {
    auto state = std::make_shared<MockSupervisor::SharedState>();
    DaemonPool pool;
    pool.set_supervisor_factory_for_test([&] {
        return std::unique_ptr<IDaemonSupervisor>(new MockSupervisor(state));
    });

    auto a = make_request("h-shared");
    a.context_id = "default";
    a.run_dir = "/run/default";
    auto b = make_request("h-shared");
    b.context_id = "resume-20260502-010203-abcd-1234abcd";
    b.run_dir = "/run/resume";

    auto ra = pool.activate(a);
    auto rb = pool.activate(b);
    EXPECT_TRUE(ra.ok) << ra.error;
    EXPECT_TRUE(rb.ok) << rb.error;
    EXPECT_EQ(state->spawn_calls.load(), 2);
    EXPECT_NE(ra.port, rb.port);
    EXPECT_NE(ra.token, rb.token);

    EXPECT_EQ(pool.lookup("h-shared", "default").state, DaemonState::Running);
    EXPECT_EQ(pool.lookup("h-shared", b.context_id).state, DaemonState::Running);
    {
        std::lock_guard<std::mutex> lk(state->mu);
        ASSERT_EQ(state->run_dirs.size(), 2u);
        EXPECT_NE(state->run_dirs[0], state->run_dirs[1]);
    }
}

// 场景: spawn 失败 → state 变 Failed,error 透传给 caller;再 activate 直接返回缓存错误
TEST(DaemonPool, ActivateSpawnFailureCachesError) {
    auto state = std::make_shared<MockSupervisor::SharedState>();
    state->spawn_should_succeed = false;
    DaemonPool pool;
    pool.set_supervisor_factory_for_test([&] {
        return std::unique_ptr<IDaemonSupervisor>(new MockSupervisor(state));
    });

    auto r1 = pool.activate(make_request("h-fail"));
    EXPECT_FALSE(r1.ok);
    EXPECT_FALSE(r1.error.empty());

    auto snap = pool.lookup("h-fail");
    EXPECT_EQ(snap.state, DaemonState::Failed);

    // 再次 activate 不重试,直接返回 cached error。spawn 总次数仍是 1。
    auto r2 = pool.activate(make_request("h-fail"));
    EXPECT_FALSE(r2.ok);
    EXPECT_EQ(state->spawn_calls.load(), 1);
}

// 场景: wait_until_ready 失败 → state Failed,且 supervisor.stop 被调一次回收资源
TEST(DaemonPool, ActivateReadyFailureTriggersStop) {
    auto state = std::make_shared<MockSupervisor::SharedState>();
    state->ready_should_succeed = false;
    DaemonPool pool;
    pool.set_supervisor_factory_for_test([&] {
        return std::unique_ptr<IDaemonSupervisor>(new MockSupervisor(state));
    });

    auto r = pool.activate(make_request("h-not-ready"));
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(pool.lookup("h-not-ready").state, DaemonState::Failed);
    EXPECT_EQ(state->stop_calls.load(), 1) << "失败路径应回收已 spawn 的进程";
}

// 场景: stop_all 调到所有 slot 的 supervisor.stop;失败列表为空表示全部 OK
TEST(DaemonPool, StopAllInvokesEverySlot) {
    auto state = std::make_shared<MockSupervisor::SharedState>();
    DaemonPool pool;
    pool.set_supervisor_factory_for_test([&] {
        return std::unique_ptr<IDaemonSupervisor>(new MockSupervisor(state));
    });
    pool.activate(make_request("a"));
    pool.activate(make_request("b"));
    pool.activate(make_request("c"));
    EXPECT_EQ(state->stop_calls.load(), 0);

    auto fails = pool.stop_all();
    EXPECT_TRUE(fails.empty());
    EXPECT_EQ(state->stop_calls.load(), 3);

    // stop_all 后状态都回到 Stopped,port/token 清空
    for (const auto& [h, snap] : pool.snapshot_all()) {
        EXPECT_EQ(snap.state, DaemonState::Stopped);
        EXPECT_EQ(snap.port, 0);
        EXPECT_TRUE(snap.token.empty());
    }
}

// 场景: missing fields 直接被 reject,不创建 slot
TEST(DaemonPool, ActivateRejectsMissingFields) {
    DaemonPool pool;
    ActivateRequest req;
    req.hash = "x"; // cwd 与 daemon_exe_path 缺失
    auto r = pool.activate(req);
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.empty());

    // slot 仍未创建
    EXPECT_TRUE(pool.snapshot_all().empty());
}
