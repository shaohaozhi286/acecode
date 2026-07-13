// 覆盖 src/session/event_dispatcher.cpp。
// EventDispatcher 是 Section 7 SessionClient/AgentLoop 事件分发的基础组件,
// 它的正确性直接影响 daemon HTTP/WebSocket 客户端能否拿到完整事件流。
// 一旦回归,断线重连补齐 / 多客户端订阅 / seq 单调性都会出错。

#include <gtest/gtest.h>

#include "session/event_dispatcher.hpp"

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

using acecode::EventDispatcher;
using acecode::SessionEvent;
using acecode::SessionEventKind;

// 场景: emit 必须分配单调递增的 seq,从 1 开始。这是 since_seq 重连补齐的根本前提。
TEST(EventDispatcher, EmitAssignsMonotonicSeqStartingAt1) {
    EventDispatcher d;
    auto s1 = d.emit(SessionEventKind::Token, {{"text", "a"}});
    auto s2 = d.emit(SessionEventKind::Token, {{"text", "b"}});
    auto s3 = d.emit(SessionEventKind::Done, {});
    EXPECT_EQ(s1, 1u);
    EXPECT_EQ(s2, 2u);
    EXPECT_EQ(s3, 3u);
    EXPECT_EQ(d.current_seq(), 3u);
}

// 场景: 注册的 listener 必须收到所有后续事件。
TEST(EventDispatcher, ListenerReceivesSubsequentEvents) {
    EventDispatcher d;
    std::vector<SessionEvent> got;
    auto sub = d.subscribe([&](const SessionEvent& e) { got.push_back(e); });
    d.emit(SessionEventKind::Token, {{"text", "x"}});
    d.emit(SessionEventKind::Done, {});
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0].kind, SessionEventKind::Token);
    EXPECT_EQ(got[1].kind, SessionEventKind::Done);
    d.unsubscribe(sub);
}

// 场景: subscribe(since_seq=N) 必须先回放 seq > N 的旧事件。
// 这是 WebSocket 断线重连的核心契约 —— 客户端记得自己看到 seq=5,断线重连
// 时传 since=5,服务端回放 6,7,8... 让客户端不丢消息。
TEST(EventDispatcher, SubscribeReplaysCachedEventsAboveSinceSeq) {
    EventDispatcher d;
    d.emit(SessionEventKind::Token, {{"text", "a"}}); // seq=1
    d.emit(SessionEventKind::Token, {{"text", "b"}}); // seq=2
    d.emit(SessionEventKind::Token, {{"text", "c"}}); // seq=3

    std::vector<SessionEvent> got;
    d.subscribe([&](const SessionEvent& e) { got.push_back(e); }, /*since_seq=*/1);
    ASSERT_EQ(got.size(), 2u) << "应回放 seq=2,3 共两条";
    EXPECT_EQ(got[0].seq, 2u);
    EXPECT_EQ(got[1].seq, 3u);
}

// 场景: since_seq=0(默认)不回放任何缓存事件,只接后续新事件。
TEST(EventDispatcher, SubscribeWithZeroSinceDoesNotReplay) {
    EventDispatcher d;
    d.emit(SessionEventKind::Token, {{"text", "old"}});
    std::vector<SessionEvent> got;
    d.subscribe([&](const SessionEvent& e) { got.push_back(e); });
    EXPECT_TRUE(got.empty()) << "since=0 不应回放";
    d.emit(SessionEventKind::Token, {{"text", "new"}});
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].payload["text"], "new");
}

// 场景: 缓存容量超出后,旧事件被淘汰,subscribe 拿不到太老的。
TEST(EventDispatcher, BufferEvictsOldEventsBeyondCapacity) {
    EventDispatcher d(/*buffer_capacity=*/3);
    for (int i = 0; i < 5; ++i) {
        d.emit(SessionEventKind::Token, {{"i", i}});
    }
    // seq 1, 2 应已被淘汰,只剩 3, 4, 5
    std::vector<SessionEvent> got;
    d.subscribe([&](const SessionEvent& e) { got.push_back(e); }, /*since_seq=*/0);
    // since=0 不回放
    EXPECT_TRUE(got.empty());

    std::vector<SessionEvent> got2;
    d.subscribe([&](const SessionEvent& e) { got2.push_back(e); }, /*since_seq=*/2);
    // 缓存里只剩 seq 3,4,5,since=2 应当回放 3,4,5
    ASSERT_EQ(got2.size(), 3u);
    EXPECT_EQ(got2[0].seq, 3u);
    EXPECT_EQ(got2[2].seq, 5u);
}

// 场景: 非 durable progress 事件应分配 seq 并投递给 live listener,但不进 replay buffer。
// 这样频繁的 agent_progress 不会挤掉 message/tool_end 这类 durable 事件。
TEST(EventDispatcher, NonBufferedEventsAreLiveOnlyAndNotReplayed) {
    EventDispatcher d;
    std::vector<SessionEvent> live;
    d.subscribe([&](const SessionEvent& e) { live.push_back(e); });

    EventDispatcher::EmitOptions opts;
    opts.buffered = false;
    auto seq = d.emit(SessionEventKind::AgentProgress,
        {{"phase", "model_waiting"}, {"label", "waiting"}}, opts);

    ASSERT_EQ(live.size(), 1u);
    EXPECT_EQ(live[0].kind, SessionEventKind::AgentProgress);
    EXPECT_EQ(live[0].seq, seq);
    EXPECT_EQ(d.current_seq(), seq);

    std::vector<SessionEvent> replay;
    d.subscribe([&](const SessionEvent& e) { replay.push_back(e); }, /*since_seq=*/0);
    EXPECT_TRUE(replay.empty()) << "live-only progress 不应 replay";

    d.emit(SessionEventKind::Done, {});
    std::vector<SessionEvent> replay2;
    d.subscribe([&](const SessionEvent& e) { replay2.push_back(e); }, /*since_seq=*/seq);
    ASSERT_EQ(replay2.size(), 1u);
    EXPECT_EQ(replay2[0].kind, SessionEventKind::Done);
}

// 场景: coalesce_key 只保留最后一个 buffered progress 状态供 reconnect replay。
// live listener 仍收到全部状态变化,方便当前连接即时更新。
TEST(EventDispatcher, CoalescedBufferedEventsReplayOnlyLatestForKey) {
    EventDispatcher d;
    auto base = d.emit(SessionEventKind::Token, {{"text", "old"}});
    std::vector<SessionEvent> live;
    d.subscribe([&](const SessionEvent& e) { live.push_back(e); });

    EventDispatcher::EmitOptions opts;
    opts.buffered = true;
    opts.coalesce_key = "session:activity";
    auto s1 = d.emit(SessionEventKind::AgentProgress,
        {{"phase", "model_waiting"}, {"label", "waiting"}}, opts);
    auto s2 = d.emit(SessionEventKind::AgentProgress,
        {{"phase", "reasoning"}, {"label", "reasoning"}}, opts);
    auto s3 = d.emit(SessionEventKind::AgentProgress,
        {{"phase", "tool_planning"}, {"label", "planning"}}, opts);

    ASSERT_EQ(live.size(), 3u);
    EXPECT_EQ(live[0].seq, s1);
    EXPECT_EQ(live[2].seq, s3);

    std::vector<SessionEvent> replay;
    d.subscribe([&](const SessionEvent& e) { replay.push_back(e); }, /*since_seq=*/0);
    EXPECT_TRUE(replay.empty()) << "since=0 仍不 replay";

    std::vector<SessionEvent> replay2;
    d.subscribe([&](const SessionEvent& e) { replay2.push_back(e); }, /*since_seq=*/base);
    ASSERT_EQ(replay2.size(), 1u);
    EXPECT_EQ(replay2[0].seq, s3);
    EXPECT_EQ(replay2[0].payload["phase"], "tool_planning");
    EXPECT_EQ(d.current_seq(), s3);

    (void)s2;
}

// 场景: unsubscribe 后该 listener 不再收到事件。
TEST(EventDispatcher, UnsubscribeStopsDelivery) {
    EventDispatcher d;
    std::atomic<int> count{0};
    auto sub = d.subscribe([&](const SessionEvent&) { count++; });
    d.emit(SessionEventKind::Token, {});
    EXPECT_EQ(count.load(), 1);
    d.unsubscribe(sub);
    d.emit(SessionEventKind::Token, {});
    EXPECT_EQ(count.load(), 1) << "退订后不应再触发";
}

// 场景: 多 listener 并存,emit 必须给所有 listener 各发一份。
TEST(EventDispatcher, MultipleListenersAllReceiveEvent) {
    EventDispatcher d;
    int a = 0, b = 0;
    d.subscribe([&](const SessionEvent&) { a++; });
    d.subscribe([&](const SessionEvent&) { b++; });
    d.emit(SessionEventKind::Token, {});
    EXPECT_EQ(a, 1);
    EXPECT_EQ(b, 1);
    EXPECT_EQ(d.listener_count(), 2u);
}

// 回归: subscribe 回放历史事件期间,并发到达的实时事件必须排在历史事件之后、
// 按 seq 顺序投递,不得乱序 / 丢失 / 重复。
//
// bug 表现(修复前): EventDispatcher::subscribe 在锁外才回放历史事件,而 listener
// 一注册进活跃列表,worker 线程的 emit 就能并发直投实时帧。于是实时新帧(seq 大)
// 可能抢在历史旧帧(seq 小)之前送达 listener。客户端按 seq 单调收,先收到大 seq
// 会把 lastSeq 顶高,随后到的小 seq(恰好可能是那条 "assistant 最终完整消息" 帧)
// 被判为过期直接丢弃 —— 表现为 desktop 上消息显示不全(如 "我发," 截断),切换
// 会话走 REST 重拉才恢复。修复后 subscribe 让该 listener 先进 catch-up 态,回放期间
// 的实时帧入 pending,回放完再按序 flush,故此处必得 2,3,4。
//
// 注:本用例用 listener 在收到 seq=2 时阻塞、主线程趁机 emit seq=4 的方式,确定性地
// 制造 "实时帧与历史回放并发" 的窗口;修复前会得到乱序的 {2,4,3}。
TEST(EventDispatcher, ConcurrentEmitDuringReplayPreservesSeqOrder) {
    EventDispatcher d;
    d.emit(SessionEventKind::Token, {{"i", 1}}); // seq=1
    d.emit(SessionEventKind::Token, {{"i", 2}}); // seq=2
    d.emit(SessionEventKind::Token, {{"i", 3}}); // seq=3

    std::vector<std::uint64_t> got;
    std::mutex got_mu;
    std::atomic<bool> in_replay{false};
    std::atomic<bool> live_emit_done{false};

    auto listener = [&](const SessionEvent& e) {
        {
            std::lock_guard<std::mutex> lk(got_mu);
            got.push_back(e.seq);
        }
        if (e.seq == 2u) {
            // 进入回放 → 通知主线程趁机 emit 实时帧,并等它 emit 完成,
            // 模拟实时帧与历史回放真实并发。注意此刻 subscribe 未持 mu_,无死锁。
            in_replay.store(true);
            while (!live_emit_done.load()) std::this_thread::yield();
        }
    };

    std::thread sub_thread([&] {
        d.subscribe(listener, /*since_seq=*/1);
    });

    while (!in_replay.load()) std::this_thread::yield();
    d.emit(SessionEventKind::Token, {{"i", 4}}); // seq=4,回放进行中到达
    live_emit_done.store(true);
    sub_thread.join();

    ASSERT_EQ(got.size(), 3u) << "应收到回放的 2,3 + 实时的 4,无丢失无重复";
    EXPECT_EQ(got[0], 2u);
    EXPECT_EQ(got[1], 3u);
    EXPECT_EQ(got[2], 4u) << "实时帧必须排在历史回放之后,严格 seq 递增";
}

// 场景: 多线程并发 emit 时,seq 仍然单调且无重复(用 atomic counter 防并发)。
// 这是 daemon 多 session 各自一个 worker thread 的关键场景。
TEST(EventDispatcher, ConcurrentEmitsHaveUniqueMonotonicSeq) {
    EventDispatcher d(/*buffer_capacity=*/100000);
    constexpr int kThreads = 4;
    constexpr int kPerThread = 500;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < kPerThread; ++i) {
                d.emit(SessionEventKind::Token, {{"i", i}});
            }
        });
    }
    for (auto& th : threads) th.join();

    EXPECT_EQ(d.current_seq(), kThreads * kPerThread);
    // 不能直接断言 seq 序列(线程 race),但 current_seq 等于总 emit 次数 +
    // 缓存里所有 seq 都是 [1, total] 不重复 = atomic 计数器工作正常的最强证明。
}

// 回归:并行工具线程会同时 emit。单个订阅的 listener 必须串行执行,且实际
// 观察到的 seq 严格递增;仅保证 seq 唯一还不够,否则 JSONL 行会逆序/交错。
TEST(EventDispatcher, ConcurrentEmitsDeliverEachListenerSeriallyInSeqOrder) {
    EventDispatcher d(/*buffer_capacity=*/100000);
    constexpr int kThreads = 6;
    constexpr int kPerThread = 120;

    std::atomic<int> active_callbacks{0};
    std::atomic<int> max_active_callbacks{0};
    std::mutex got_mu;
    std::vector<std::uint64_t> got;
    d.subscribe([&](const SessionEvent& event) {
        const int active = active_callbacks.fetch_add(1) + 1;
        int observed = max_active_callbacks.load();
        while (active > observed &&
               !max_active_callbacks.compare_exchange_weak(observed, active)) {}
        std::this_thread::yield();
        {
            std::lock_guard<std::mutex> lk(got_mu);
            got.push_back(event.seq);
        }
        active_callbacks.fetch_sub(1);
    });

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < kPerThread; ++i) {
                d.emit(SessionEventKind::Token, {{"i", i}});
            }
        });
    }
    for (auto& thread : threads) thread.join();

    ASSERT_EQ(got.size(), static_cast<std::size_t>(kThreads * kPerThread));
    EXPECT_EQ(max_active_callbacks.load(), 1);
    for (std::size_t i = 0; i < got.size(); ++i) {
        EXPECT_EQ(got[i], i + 1) << "listener delivery diverged at index " << i;
    }
}
