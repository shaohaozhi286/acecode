#include "ask_user_question_prompter.hpp"

#include "../utils/uuid.hpp"

#include <algorithm>

namespace acecode {

std::string AskUserQuestionPrompter::make_request_id() {
    return generate_uuid();
}

AskUserQuestionResponse
AskUserQuestionPrompter::prompt(const nlohmann::json& questions_payload,
                                  const std::atomic<bool>* abort_flag,
                                  std::optional<std::chrono::milliseconds> timeout_override) {
    std::string req_id = make_request_id();
    const auto effective_timeout = timeout_override.value_or(timeout_);
    const bool has_timeout = effective_timeout > std::chrono::milliseconds{0};
    const auto created_at = PendingQuestionRequestSnapshot::Clock::now();

    nlohmann::json payload;
    payload["request_id"] = req_id;
    payload["questions"]  = questions_payload;
    payload["timeout_ms"] = has_timeout ? effective_timeout.count() : 0;

    auto pending = std::make_shared<Pending>();
    pending->created_at = created_at;
    if (has_timeout) pending->deadline = created_at + effective_timeout;
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        pending->order = ++next_order_;
        payload["request_order"] = pending->order;
        pending->request_payload = payload;
        pending_[req_id] = pending;
        pending_order_.push_back(req_id);
    }

    // 推 QuestionRequest 事件;前端拿到后弹 modal,提交后通过
    // SessionClient::respond_question 回应。
    events_.emit(SessionEventKind::QuestionRequest, payload);

    AskUserQuestionResponse result;
    result.cancelled = true; // 默认值(超时 / abort 时返回)
    std::string close_reason = "aborted";

    while (true) {
        std::unique_lock<std::mutex> lk(pending->mu);
        bool got = pending->cv.wait_for(lk, std::chrono::milliseconds(50),
            [&] { return pending->responded; });

        if (got) {
            result = pending->response;
            pending->accepting = false;
            close_reason = result.cancelled ? "cancelled" : "answered";
            break;
        }
        if (abort_flag && abort_flag->load()) {
            // abort_flag 走出循环 = cancelled 默认值生效
            pending->accepting = false;
            close_reason = "aborted";
            break;
        }
        if (pending->deadline.has_value() &&
            PendingQuestionRequestSnapshot::Clock::now() >= *pending->deadline) {
            // timeout 策略的正常路径(add-ask-question-policy),不是错误:
            // 标记 timed_out 让工具侧合成自动采纳结果,cancelled 保持 false。
            pending->accepting = false;
            close_reason = "timeout";
            result.cancelled = false;
            result.timed_out = true;
            break;
        }
    }

    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        pending_.erase(req_id);
        auto order_it = std::find(pending_order_.begin(), pending_order_.end(), req_id);
        if (order_it != pending_order_.end()) pending_order_.erase(order_it);
    }
    events_.emit(SessionEventKind::QuestionClosed, nlohmann::json{
        {"request_id", req_id},
        {"reason", close_reason},
    });
    return result;
}

bool AskUserQuestionPrompter::notify_response(
    const std::string& request_id,
    const AskUserQuestionResponse& response) {
    std::shared_ptr<Pending> p;
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        auto it = pending_.find(request_id);
        if (it == pending_.end()) return false;
        p = it->second;
    }
    {
        std::lock_guard<std::mutex> lk(p->mu);
        if (p->responded || !p->accepting) return false;
        p->response = response;
        p->responded = true;
        p->accepting = false;
    }
    p->cv.notify_all();
    return true;
}

std::size_t AskUserQuestionPrompter::pending_count() const {
    std::lock_guard<std::mutex> lk(pending_mu_);
    return pending_.size();
}

std::vector<nlohmann::json>
AskUserQuestionPrompter::snapshot_pending_requests() const {
    std::vector<nlohmann::json> out;
    for (const auto& request : snapshot_pending_question_requests()) {
        nlohmann::json payload{
            {"request_id", request.request_id},
            {"questions", request.questions},
            {"request_order", request.order},
            {"timeout_ms",
             request.deadline.has_value()
                 ? std::chrono::duration_cast<std::chrono::milliseconds>(
                       *request.deadline - request.created_at)
                       .count()
                 : 0},
        };
        out.push_back(std::move(payload));
    }
    return out;
}

std::vector<PendingQuestionRequestSnapshot>
AskUserQuestionPrompter::snapshot_pending_question_requests() const {
    std::vector<std::pair<std::string, std::shared_ptr<Pending>>> snapshot;
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        snapshot.reserve(pending_order_.size());
        for (const auto& request_id : pending_order_) {
            auto it = pending_.find(request_id);
            if (it != pending_.end() && it->second) {
                snapshot.emplace_back(request_id, it->second);
            }
        }
    }

    std::vector<PendingQuestionRequestSnapshot> out;
    out.reserve(snapshot.size());
    for (const auto& [request_id, pending] : snapshot) {
        std::lock_guard<std::mutex> lk(pending->mu);
        if (pending->responded || !pending->accepting) continue;
        PendingQuestionRequestSnapshot request;
        request.request_id = request_id;
        request.questions =
            pending->request_payload.value("questions", nlohmann::json::array());
        request.order = pending->order;
        request.created_at = pending->created_at;
        request.deadline = pending->deadline;
        out.push_back(std::move(request));
    }
    return out;
}

} // namespace acecode
