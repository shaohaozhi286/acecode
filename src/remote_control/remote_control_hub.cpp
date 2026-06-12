#include "remote_control_hub.hpp"

#include "utils/logger.hpp"

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

} // namespace

nlohmann::json outbound_message_to_json(const OutboundMessage& msg) {
    return nlohmann::json{
        {"type", msg.type},
        {"session_id", msg.session_id},
        {"text", msg.text},
        {"timestamp_ms", msg.timestamp_ms},
        {"seq", msg.seq},
    };
}

RemoteControlHub::~RemoteControlHub() {
    disable();
}

void RemoteControlHub::set_inbound_submit(InboundSubmit fn) {
    std::lock_guard<std::mutex> lk(mu_);
    inbound_submit_ = std::move(fn);
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
    worker_ = std::thread([this] { worker_loop(); });
}

void RemoteControlHub::disable() {
    std::unique_lock<std::mutex> lk(mu_);
    enabled_ = false;
    stop_worker_locked(lk);
    sender_.reset();
    queue_.clear();
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

InboundResult RemoteControlHub::handle_inbound(const std::string& text,
                                               const std::string& provided_token) {
    InboundSubmit submit;
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
        if (!inbound_submit_) {
            return reject(InboundResult::Code::NoSession, "no session attached");
        }
        submit = inbound_submit_;
        ++stats_.inbound_accepted;
    }
    // 锁外调用:submit 内部会拿 TUI state.mu,持 mu_ 调用有死锁风险。
    submit(text);
    return InboundResult{};
}

void RemoteControlHub::notify_assistant_text(const std::string& text) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!enabled_ || !sender_) return;
    if (text.empty() || is_blank(text)) return;
    OutboundMessage msg;
    msg.type = "assistant_message";
    msg.session_id = session_id_;
    msg.text = text;
    msg.timestamp_ms = now_ms();
    msg.seq = next_seq_++;
    if (queue_.size() >= kMaxQueue) {
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
    }
}

} // namespace acecode::rc
