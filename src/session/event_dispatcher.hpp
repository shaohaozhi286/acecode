#pragma once

// EventDispatcher: AgentLoop 的事件广播 + seq 编号 + 环形回放缓存。
// 每个 AgentLoop 实例持有一个 EventDispatcher,负责:
//   - 给每个 emit 分配单调递增 seq(从 1 开始)
//   - 缓存最近 N 个事件(默认 1024,token 流为主时已经够大;非-token 事件
//     远比 token 少,所以 1024 实际能覆盖很长一段对话)
//   - 维护 listener 列表(unordered_map<SubscriptionId, listener>),emit 时
//     遍历调用
//   - subscribe(since_seq) 时把缓存里 seq > since_seq 的事件按序回放给该
//     listener,再切换到实时投递(有序保证见下)
//
// 线程模型:
//   - emit() 由 AgentLoop worker 线程调用
//   - subscribe/unsubscribe 由 HTTP handler 线程调用(可能多个并发)
//   - listener 内部的回调可能跨网络发送,emit/subscribe 都不持锁调 listener
//     (把要投递的事件复制出来再调,避免 listener 阻塞影响 worker)
//
// 有序投递保证(修复 replay 与 live 抢道乱序丢帧):
//   listener 注册后立即进入 catch-up 态。这段时间 emit() 不直接调它,而是把
//   实时事件按 seq 顺序压入该订阅的 pending 队列。subscribe 线程先把历史事件
//   按序回放,再 flush pending,最后在同一把锁内翻转出 catch-up 态切到直投。
//   于是 listener 永远先收全历史、再收实时,且全程严格 seq 递增 —— 不会出现
//   "实时新帧抢在历史旧帧之前送达、被客户端按 seq 误判为过期而丢弃" 的问题。

#include "session_client.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace acecode {

class EventDispatcher {
public:
    using EventListener  = SessionClient::EventListener;
    using SubscriptionId = SessionClient::SubscriptionId;

    struct EmitOptions {
        // Buffered events are replayed to reconnecting clients via
        // subscribe(since_seq). Transient progress ticks can set this false so
        // they are delivered live without evicting durable transcript events.
        bool buffered = true;

        // When non-empty and buffered=true, only the latest buffered event for
        // the same coalesce_key is retained. Listeners still receive every live
        // event; replay clients receive the latest state only.
        std::string coalesce_key;
    };

    explicit EventDispatcher(std::size_t buffer_capacity = 1024);

    // 由 AgentLoop 调用。线程安全。返回分配的 seq(从 1 开始)。
    std::uint64_t emit(SessionEventKind kind, nlohmann::json payload);
    std::uint64_t emit(SessionEventKind kind, nlohmann::json payload, EmitOptions options);

    // 注册一个 listener。`since_seq` > 0 时先按 seq 顺序回放缓存里
    // seq > since_seq 的事件给这个 listener,再切到实时投递;回放期间产生的
    // 实时事件会被排队并在回放后按序补发(见文件头 "有序投递保证")。返回 id
    // 供 unsubscribe 用。线程安全。
    SubscriptionId subscribe(EventListener listener, std::uint64_t since_seq = 0);

    // 退订。线程安全;退订一个不存在的 id 是 no-op。
    void unsubscribe(SubscriptionId id);

    // 当前最大 seq(用于客户端记录"我看到哪了")。
    std::uint64_t current_seq() const { return seq_counter_.load(); }

    // 测试用: 当前活跃 listener 数。
    std::size_t listener_count() const;

private:
    // 单个订阅的投递状态。catching_up=true 表示 subscribe 还在按序回放历史事件,
    // 此时 emit() 把实时事件压入 pending 而非直投;回放结束后由 subscribe 线程
    // flush pending 并在同一把锁内翻转 catching_up=false 切到直投。
    struct Subscription {
        EventListener             listener;
        bool                      catching_up = false;
        std::vector<SessionEvent> pending;
    };

    void push_to_buffer(const SessionEvent& evt, const std::string& coalesce_key = {});

    std::atomic<std::uint64_t> seq_counter_{0};
    std::atomic<SubscriptionId> next_sub_id_{1};
    std::size_t                buffer_capacity_;

    mutable std::mutex                                                mu_;
    std::deque<SessionEvent>                                          buffer_;
    std::unordered_map<std::string, std::uint64_t>                    coalesced_seq_by_key_;
    std::unordered_map<SubscriptionId, std::shared_ptr<Subscription>> subscriptions_;
};

} // namespace acecode
