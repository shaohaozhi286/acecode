#include "event_dispatcher.hpp"

#include "../utils/logger.hpp"

#include <chrono>
#include <utility>

namespace acecode {

EventDispatcher::EventDispatcher(std::size_t buffer_capacity)
    : buffer_capacity_(buffer_capacity == 0 ? 1 : buffer_capacity) {}

std::uint64_t EventDispatcher::emit(SessionEventKind kind, nlohmann::json payload) {
    return emit(kind, std::move(payload), EmitOptions{});
}

std::uint64_t EventDispatcher::emit(SessionEventKind kind, nlohmann::json payload,
                                    EmitOptions options) {
    SessionEvent evt;
    evt.kind         = kind;
    evt.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    evt.payload      = std::move(payload);

    // seq 分配与所有订阅入队在同一把锁下完成:并发 emitter 即使在拿锁顺序上
    // 竞争,每个订阅 pending 的物理顺序也必然与 seq 一致。
    std::vector<std::pair<SubscriptionId, std::shared_ptr<Subscription>>> drain_targets;
    {
        std::lock_guard<std::mutex> lk(mu_);
        evt.seq = ++seq_counter_;
        if (options.buffered) {
            push_to_buffer(evt, options.coalesce_key);
        }
        drain_targets.reserve(subscriptions_.size());
        for (auto& [id, sub] : subscriptions_) {
            if (!sub) continue;
            sub->pending.push_back(evt);
            if (!sub->catching_up && !sub->delivering && sub->listener) {
                sub->delivering = true;
                drain_targets.emplace_back(id, sub);
            }
        }
    }
    for (auto& [id, sub] : drain_targets) {
        drain_subscription(id, sub);
    }
    return evt.seq;
}

void EventDispatcher::drain_subscription(
    SubscriptionId id,
    const std::shared_ptr<Subscription>& sub) {
    while (true) {
        EventListener listener;
        SessionEvent evt;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = subscriptions_.find(id);
            if (it == subscriptions_.end() || it->second != sub || sub->catching_up) {
                sub->delivering = false;
                return;
            }
            if (sub->pending.empty()) {
                sub->delivering = false;
                return;
            }
            evt = std::move(sub->pending.front());
            sub->pending.pop_front();
            listener = sub->listener;
        }
        if (listener) listener(evt);
    }
}

EventDispatcher::SubscriptionId
EventDispatcher::subscribe(EventListener listener, std::uint64_t since_seq) {
    SubscriptionId id = next_sub_id_.fetch_add(1);

    auto sub = std::make_shared<Subscription>();
    sub->listener     = listener;
    sub->catching_up  = true;

    // 第一步(持锁): 快照需要回放的历史事件 + 把订阅注册为 catch-up 态。
    // 注册后,emit() 会把实时事件压进 sub->pending 而非直投。
    std::vector<SessionEvent> to_replay;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (since_seq > 0) {
            for (const auto& evt : buffer_) {
                if (evt.seq > since_seq) to_replay.push_back(evt);
            }
        }
        subscriptions_[id] = sub;
    }

    // 第二步(锁外): 按 seq 顺序回放历史事件。期间产生的实时事件都进了 pending。
    if (listener) {
        for (const auto& evt : to_replay) listener(evt);
    }

    // 第三步:按序 flush catch-up 期间累积的实时事件;在 pending 清空的同一把锁内
    // 翻转 catching_up=false。之后 emit 会认领 delivering 并从同一个队列继续 drain。
    std::size_t live_buffered = 0;
    while (true) {
        SessionEvent evt;
        bool have_event = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = subscriptions_.find(id);
            if (it == subscriptions_.end()) break;   // 回放途中被 unsubscribe
            if (it->second->pending.empty()) {
                it->second->catching_up = false;
                break;
            }
            evt = std::move(it->second->pending.front());
            it->second->pending.pop_front();
            have_event = true;
        }
        if (have_event && listener) listener(evt);
        if (have_event) ++live_buffered;
    }

    // 只在真正触发了回放 / catch-up 排队时记一条 —— 平时(since=0 且无并发)零噪声。
    // live_buffered>0 即命中了 "实时事件与历史回放并发" 的窗口,正是过去导致
    // 乱序丢帧的场景,现在被有序补发正确吸收;留作回归观测信号。
    if (since_seq > 0 || live_buffered > 0) {
        LOG_DEBUG("[event_dispatcher] subscribe id=" + std::to_string(id) +
                  " since=" + std::to_string(since_seq) +
                  " replayed=" + std::to_string(to_replay.size()) +
                  " live_buffered=" + std::to_string(live_buffered) +
                  " current_seq=" + std::to_string(seq_counter_.load()));
    }

    return id;
}

void EventDispatcher::unsubscribe(SubscriptionId id) {
    std::lock_guard<std::mutex> lk(mu_);
    subscriptions_.erase(id);
}

std::size_t EventDispatcher::listener_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return subscriptions_.size();
}

void EventDispatcher::push_to_buffer(const SessionEvent& evt, const std::string& coalesce_key) {
    // 调用方持锁
    if (!coalesce_key.empty()) {
        auto it = coalesced_seq_by_key_.find(coalesce_key);
        if (it != coalesced_seq_by_key_.end()) {
            const std::uint64_t old_seq = it->second;
            for (auto bit = buffer_.begin(); bit != buffer_.end(); ++bit) {
                if (bit->seq == old_seq) {
                    buffer_.erase(bit);
                    break;
                }
            }
        }
        coalesced_seq_by_key_[coalesce_key] = evt.seq;
    }
    buffer_.push_back(evt);
    while (buffer_.size() > buffer_capacity_) {
        const auto evicted_seq = buffer_.front().seq;
        for (auto it = coalesced_seq_by_key_.begin(); it != coalesced_seq_by_key_.end();) {
            if (it->second == evicted_seq) it = coalesced_seq_by_key_.erase(it);
            else ++it;
        }
        buffer_.pop_front();
    }
}

} // namespace acecode
