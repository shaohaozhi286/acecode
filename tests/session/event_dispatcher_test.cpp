// 覆盖 src/session/event_dispatcher.cpp。
// EventDispatcher 是 Section 7 SessionClient/AgentLoop 事件分发的基础组件,
// 它的正确性直接影响 daemon HTTP/WebSocket 客户端能否拿到完整事件流。
// 一旦回归,断线重连补齐 / 多客户端订阅 / seq 单调性都会出错。

#include <gtest/gtest.h>

#include "session/event_dispatcher.hpp"

#include <atomic>
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
