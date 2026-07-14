#include "remote_control_hub.hpp"

#include "outbound_summary.hpp"
#include "utils/logger.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>

namespace acecode::rc {

namespace {

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool is_blank(const std::string& s) {
    for (unsigned char c : s) {
        if (!std::isspace(c)) return false;
    }
    return true;
}

constexpr const char* kInboundAcknowledgement = "思考中...";
constexpr auto kInboundRouteDrainTimeout = std::chrono::seconds(5);

} // namespace

nlohmann::json outbound_message_to_json(const OutboundMessage& msg) {
    nlohmann::json j{
        {"type", msg.type},
        {"session_id", msg.session_id},
        {"timestamp_ms", msg.timestamp_ms},
        {"seq", msg.seq},
    };
    // 可选字段:留空即代表"不适用于本消息类型",不序列化该键 —— channel
    // bridge 侧按键是否存在分支,而不是按空字符串分支。
    if (!msg.text.empty()) j["text"] = msg.text;
    if (!msg.tool_name.empty()) j["tool_name"] = msg.tool_name;
    if (!msg.args_preview.empty()) j["args_preview"] = msg.args_preview;
    if (!msg.in_reply_to.empty()) j["in_reply_to"] = msg.in_reply_to;
    return j;
}

RemoteControlHub::~RemoteControlHub() {
    disable();
}

void RemoteControlHub::set_inbound_submit(InboundSubmit fn) {
    std::lock_guard<std::mutex> lk(mu_);
    inbound_submit_ = std::move(fn);
}

void RemoteControlHub::set_inbound_route(std::string session_id,
                                         InboundSubmit fn) {
    std::lock_guard<std::mutex> lk(mu_);
    if (session_id.empty() || !fn) {
        session_id_.clear();
        inbound_submit_ = {};
        return;
    }
    session_id_ = std::move(session_id);
    inbound_submit_ = std::move(fn);
}

void RemoteControlHub::clear_inbound_route() {
    std::unique_lock<std::mutex> lk(mu_);
    session_id_.clear();
    inbound_submit_ = {};

    // sender 未安装时 worker 不能 dequeue；worker 未运行时也没有进展者。
    // 这两种情况下不能把清路变成无限等待。已在队列中的消息仍遵循原有
    // sender 安装/enable 生命周期。
    if (!worker_.joinable() || !sender_) return;

    // 只等待调用瞬间真实留在 FIFO 中的尾消息。next_seq_ 也会为因
    // drain barrier 满队列而拒绝的新消息递增；以它为目标会等待一个
    // 从未入队、因而永远不会 dequeue 的 seq。
    if (queue_.empty()) return;
    const std::uint64_t barrier_seq = queue_.back().seq;
    if (barrier_seq <= last_dequeued_seq_) return;
    drain_through_seq_ = std::max(drain_through_seq_, barrier_seq);
    cv_.notify_all();
    cv_.wait_for(lk, kInboundRouteDrainTimeout, [this, barrier_seq] {
        return last_dequeued_seq_ >= barrier_seq || !sender_ || stopping_ ||
               !worker_.joinable();
    });
    if (last_dequeued_seq_ < barrier_seq) {
        // 默认 HTTP sender 自带 3 秒超时；额外的 5 秒上限约束 barrier
        // 自身增加的等待。worker join 仍依赖 OutboundSender 的有限返回契约。
        // 超时后关闭优先，尚未被 worker 接管的尾部消息按既有 disable 语义清理。
        if (drain_through_seq_ <= barrier_seq) drain_through_seq_ = 0;
        LOG_WARN("[remote-control] timed out draining outbound queue through seq=" +
                 std::to_string(barrier_seq));
    }
}

void RemoteControlHub::enable(std::string token,
                              std::string session_id,
                              std::shared_ptr<OutboundSender> sender) {
    std::unique_lock<std::mutex> lk(mu_);
    stop_worker_locked(lk);
    enabled_ = true;
    stopping_ = false;
    token_ = std::move(token);
    session_id_ = std::move(session_id);
    sender_ = std::move(sender);
    queue_.clear();
    // seq 跨 enable 生命周期保持单调；被本次重建清掉的旧消息应被视作
    // 已越过，避免一次没有新出站的 clear barrier 等待不存在的 seq。
    last_dequeued_seq_ = next_seq_ - 1;
    drain_through_seq_ = 0;
    worker_ = std::thread([this] { worker_loop(); });
}

void RemoteControlHub::disable() {
    std::unique_lock<std::mutex> lk(mu_);
    enabled_ = false;
    stop_worker_locked(lk);
    sender_.reset();
    queue_.clear();
    drain_through_seq_ = 0;
}

void RemoteControlHub::stop_worker_locked(std::unique_lock<std::mutex>& lk) {
    if (!worker_.joinable()) return;
    stopping_ = true;
    cv_.notify_all();
    // join 必须放锁外,worker 退出前还要拿一次锁。
    std::thread to_join = std::move(worker_);
    lk.unlock();
    to_join.join();
    lk.lock();
    stopping_ = false;
}

bool RemoteControlHub::enabled() const {
    std::lock_guard<std::mutex> lk(mu_);
    return enabled_;
}

std::string RemoteControlHub::token() const {
    std::lock_guard<std::mutex> lk(mu_);
    return token_;
}

void RemoteControlHub::set_outbound_sender(std::shared_ptr<OutboundSender> sender) {
    std::lock_guard<std::mutex> lk(mu_);
    sender_ = std::move(sender);
    cv_.notify_all();
}

void RemoteControlHub::set_session_id(std::string session_id) {
    std::lock_guard<std::mutex> lk(mu_);
    session_id_ = std::move(session_id);
}

void RemoteControlHub::set_outbound_result_observer(OutboundResultObserver observer) {
    std::lock_guard<std::mutex> lk(mu_);
    outbound_result_observer_ = std::move(observer);
}

InboundResult RemoteControlHub::handle_inbound(const std::string& text,
                                               const std::string& provided_token) {
    InboundSubmit submit;
    std::string route_session_id;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto reject = [this](InboundResult::Code code, std::string msg) {
            ++stats_.inbound_rejected;
            return InboundResult{code, std::move(msg)};
        };
        if (!enabled_) {
            return reject(InboundResult::Code::Disabled, "remote control is off");
        }
        if (provided_token.empty() || provided_token != token_) {
            return reject(InboundResult::Code::BadToken, "invalid token");
        }
        if (text.empty() || is_blank(text)) {
            return reject(InboundResult::Code::BadText, "text must be non-empty");
        }
        if (text.size() > kMaxInboundBytes) {
            return reject(InboundResult::Code::BadText,
                          "text exceeds " + std::to_string(kMaxInboundBytes) + " bytes");
        }
        if (session_id_.empty() || !inbound_submit_) {
            return reject(InboundResult::Code::NoSession, "no session attached");
        }
        route_session_id = session_id_;
        submit = inbound_submit_;
        ++stats_.inbound_accepted;
        // 必须在 submit 前进入同一出站 FIFO:submit 可能立即启动模型或做
        // 协调工作,但确认不能被这些工作拖延。sender 尚未就绪时也保留在
        // 有界队列中,待 set_outbound_sender 后由 hub worker 异步投递。
        enqueue_assistant_text_locked(kInboundAcknowledgement, route_session_id);
    }
    // 锁外调用:submit 内部会拿 TUI state.mu,持 mu_ 调用有死锁风险。
    submit(text);
    return InboundResult{};
}

void RemoteControlHub::notify_assistant_text(const std::string& text) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!enabled_ || !sender_) return;
    if (text.empty() || is_blank(text)) return;
    enqueue_assistant_text_locked(text, session_id_);
}

void RemoteControlHub::enqueue_assistant_text_locked(
    const std::string& text, const std::string& session_id) {
    OutboundMessage msg;
    msg.type = "assistant_message";
    msg.session_id = session_id;
    msg.text = text;
    msg.timestamp_ms = now_ms();
    msg.seq = next_seq_++;
    if (queue_.size() >= kMaxQueue) {
        if (drain_through_seq_ != 0 &&
            queue_.front().seq <= drain_through_seq_) {
            ++stats_.outbound_dropped;
            return;
        }
        queue_.pop_front();
        ++stats_.outbound_dropped;
    }
    queue_.push_back(std::move(msg));
    cv_.notify_all();
}

void RemoteControlHub::notify_tool_call(const std::string& session_id,
                                        const std::string& tool_name,
                                        const nlohmann::json& arguments) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!enabled_ || !sender_) return;
    OutboundMessage msg;
    msg.type = "tool_call";
    msg.session_id = session_id;
    msg.tool_name = tool_name;
    msg.args_preview = summarize_tool_args(tool_name, arguments);
    msg.timestamp_ms = now_ms();
    msg.seq = next_seq_++;
    if (queue_.size() >= kMaxQueue) {
        if (drain_through_seq_ != 0 &&
            queue_.front().seq <= drain_through_seq_) {
            ++stats_.outbound_dropped;
            return;
        }
        queue_.pop_front();
        ++stats_.outbound_dropped;
    }
    queue_.push_back(std::move(msg));
    cv_.notify_all();
}

void RemoteControlHub::set_forward_cursor(std::size_t index) {
    std::lock_guard<std::mutex> lk(mu_);
    forward_cursor_ = index;
}

std::size_t RemoteControlHub::forward_cursor() const {
    std::lock_guard<std::mutex> lk(mu_);
    return forward_cursor_;
}

RemoteControlStats RemoteControlHub::stats() const {
    std::lock_guard<std::mutex> lk(mu_);
    return stats_;
}

void RemoteControlHub::worker_loop() {
    std::unique_lock<std::mutex> lk(mu_);
    while (true) {
        cv_.wait(lk, [this] {
            return stopping_ || (!queue_.empty() && sender_ != nullptr);
        });
        if (stopping_) return;
        OutboundMessage msg = std::move(queue_.front());
        queue_.pop_front();
        last_dequeued_seq_ = msg.seq;
        if (drain_through_seq_ != 0 &&
            last_dequeued_seq_ >= drain_through_seq_) {
            drain_through_seq_ = 0;
        }
        cv_.notify_all();
        std::shared_ptr<OutboundSender> sender = sender_;
        lk.unlock();

        std::string error;
        const bool ok = sender->send(msg, &error);

        lk.lock();
        if (ok) {
            ++stats_.outbound_sent;
        } else {
            ++stats_.outbound_failed;
            LOG_WARN("[remote-control] outbound send failed (seq=" +
                     std::to_string(msg.seq) + "): " + error);
        }
        // 结果观察者:拷贝后锁外调用,观察者内部可能回头拿别的锁
        // (daemon 保活判定),持 mu_ 调用有锁序风险。
        OutboundResultObserver observer = outbound_result_observer_;
        if (observer) {
            lk.unlock();
            observer(ok);
            lk.lock();
        }
    }
}

} // namespace acecode::rc
