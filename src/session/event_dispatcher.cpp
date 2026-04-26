#include "event_dispatcher.hpp"

#include <chrono>
#include <utility>

namespace acecode {

EventDispatcher::EventDispatcher(std::size_t buffer_capacity)
    : buffer_capacity_(buffer_capacity == 0 ? 1 : buffer_capacity) {}

std::uint64_t EventDispatcher::emit(SessionEventKind kind, nlohmann::json payload) {
    SessionEvent evt;
    evt.kind         = kind;
    evt.seq          = ++seq_counter_;
    evt.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    evt.payload      = std::move(payload);

    // 关键: 把 listener 复制出来再调,避免在持锁状态下调用客户代码
    // (listener 可能跨网络发包阻塞)。
    std::vector<EventListener> snapshot;
    {
        std::lock_guard<std::mutex> lk(mu_);
        push_to_buffer(evt);
        snapshot.reserve(listeners_.size());
        for (auto& [_, l] : listeners_) snapshot.push_back(l);
    }
    for (auto& l : snapshot) {
        if (l) l(evt);
    }
    return evt.seq;
}

EventDispatcher::SubscriptionId
EventDispatcher::subscribe(EventListener listener, std::uint64_t since_seq) {
    SubscriptionId id = next_sub_id_.fetch_add(1);

    // 回放缓存里 seq > since_seq 的事件,然后再把 listener 注册进去。
    // 必须在同一把锁内完成 replay → register,否则 emit 可能在两步之间
    // 插入新事件导致 listener 漏看 / 重复看。
    std::vector<SessionEvent> to_replay;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (since_seq > 0) {
            for (const auto& evt : buffer_) {
                if (evt.seq > since_seq) to_replay.push_back(evt);
            }
        }
        listeners_[id] = listener;
    }
    if (listener) {
        for (const auto& evt : to_replay) listener(evt);
    }
    return id;
}

void EventDispatcher::unsubscribe(SubscriptionId id) {
    std::lock_guard<std::mutex> lk(mu_);
    listeners_.erase(id);
}

std::size_t EventDispatcher::listener_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return listeners_.size();
}

void EventDispatcher::push_to_buffer(const SessionEvent& evt) {
    // 调用方持锁
    buffer_.push_back(evt);
    while (buffer_.size() > buffer_capacity_) buffer_.pop_front();
}

} // namespace acecode
