#pragma once

// EventDispatcher: AgentLoop 的事件广播 + seq 编号 + 环形回放缓存。
// 每个 AgentLoop 实例持有一个 EventDispatcher,负责:
//   - 给每个 emit 分配单调递增 seq(从 1 开始)
//   - 缓存最近 N 个事件(默认 1024,token 流为主时已经够大;非-token 事件
//     远比 token 少,所以 1024 实际能覆盖很长一段对话)
//   - 维护 listener 列表(unordered_map<SubscriptionId, listener>),emit 时
//     遍历调用
//   - subscribe(since_seq) 时立刻把缓存里 seq > since_seq 的事件回放给该
//     listener,然后才把它加入活跃列表(原子操作,确保不漏不重)
//
// 线程模型:
//   - emit() 由 AgentLoop worker 线程调用
//   - subscribe/unsubscribe 由 HTTP handler 线程调用(可能多个并发)
//   - listener 内部的回调可能跨网络发送,emit 不持锁调 listener(把 listener
//     vector 复制出来再调,避免 listener 阻塞影响 worker)

#include "session_client.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace acecode {

class EventDispatcher {
public:
    using EventListener  = SessionClient::EventListener;
    using SubscriptionId = SessionClient::SubscriptionId;

    explicit EventDispatcher(std::size_t buffer_capacity = 1024);

    // 由 AgentLoop 调用。线程安全。返回分配的 seq(从 1 开始)。
    std::uint64_t emit(SessionEventKind kind, nlohmann::json payload);

    // 注册一个 listener。`since_seq` > 0 时立刻回放缓存里 seq > since_seq
    // 的事件给这个 listener(同步调用),然后把它加入活跃列表。返回 id 供
    // unsubscribe 用。线程安全。
    SubscriptionId subscribe(EventListener listener, std::uint64_t since_seq = 0);

    // 退订。线程安全;退订一个不存在的 id 是 no-op。
    void unsubscribe(SubscriptionId id);

    // 当前最大 seq(用于客户端记录"我看到哪了")。
    std::uint64_t current_seq() const { return seq_counter_.load(); }

    // 测试用: 当前活跃 listener 数。
    std::size_t listener_count() const;

private:
    void push_to_buffer(const SessionEvent& evt);

    std::atomic<std::uint64_t> seq_counter_{0};
    std::atomic<SubscriptionId> next_sub_id_{1};
    std::size_t                buffer_capacity_;

    mutable std::mutex                                   mu_;
    std::deque<SessionEvent>                             buffer_;
    std::unordered_map<SubscriptionId, EventListener>   listeners_;
};

} // namespace acecode
