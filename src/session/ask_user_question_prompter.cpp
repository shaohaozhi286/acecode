#include "ask_user_question_prompter.hpp"

#include "../utils/uuid.hpp"

namespace acecode {

std::string AskUserQuestionPrompter::make_request_id() {
    return generate_uuid();
}

AskUserQuestionResponse
AskUserQuestionPrompter::prompt(const nlohmann::json& questions_payload,
                                  const std::atomic<bool>* abort_flag,
                                  std::optional<std::chrono::milliseconds> timeout_override) {
    std::string req_id = make_request_id();

    nlohmann::json payload;
    payload["request_id"] = req_id;
    payload["questions"]  = questions_payload;

    auto pending = std::make_shared<Pending>();
    pending->request_payload = payload;
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        pending_[req_id] = pending;
    }

    // 推 QuestionRequest 事件;前端拿到后弹 modal,提交后通过
    // SessionClient::respond_question 回应。
    events_.emit(SessionEventKind::QuestionRequest, payload);

    const auto effective_timeout = timeout_override.value_or(timeout_);
    const bool has_timeout = effective_timeout > std::chrono::milliseconds{0};
    auto deadline = std::chrono::steady_clock::now() + effective_timeout;
    AskUserQuestionResponse result;
    result.cancelled = true; // 默认值(超时 / abort 时返回)
    std::string close_reason = "aborted";

    while (true) {
        std::unique_lock<std::mutex> lk(pending->mu);
        bool got = pending->cv.wait_for(lk, std::chrono::milliseconds(50),
            [&] { return pending->responded; });

        if (got) {
            result = pending->response;
            close_reason = result.cancelled ? "cancelled" : "answered";
            break;
        }
        if (abort_flag && abort_flag->load()) {
            // abort_flag 走出循环 = cancelled 默认值生效
            close_reason = "aborted";
            break;
        }
        if (has_timeout && std::chrono::steady_clock::now() >= deadline) {
            // timeout 策略的正常路径(add-ask-question-policy),不是错误:
            // 标记 timed_out 让工具侧合成自动采纳结果,cancelled 保持 false。
            close_reason = "timeout";
            result.cancelled = false;
            result.timed_out = true;
            break;
        }
    }

    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        pending_.erase(req_id);
    }
    events_.emit(SessionEventKind::QuestionClosed, nlohmann::json{
        {"request_id", req_id},
        {"reason", close_reason},
    });
    return result;
}

void AskUserQuestionPrompter::notify_response(const std::string& request_id,
                                                  const AskUserQuestionResponse& response) {
    std::shared_ptr<Pending> p;
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        auto it = pending_.find(request_id);
        if (it == pending_.end()) return;
        p = it->second;
    }
    {
        std::lock_guard<std::mutex> lk(p->mu);
        if (p->responded) return;
        p->response = response;
        p->responded = true;
    }
    p->cv.notify_all();
}

std::size_t AskUserQuestionPrompter::pending_count() const {
    std::lock_guard<std::mutex> lk(pending_mu_);
    return pending_.size();
}

std::vector<nlohmann::json>
AskUserQuestionPrompter::snapshot_pending_requests() const {
    std::vector<std::pair<std::string, std::shared_ptr<Pending>>> snapshot;
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        snapshot.reserve(pending_.size());
        for (const auto& [request_id, pending] : pending_) {
            if (pending) snapshot.emplace_back(request_id, pending);
        }
    }

    std::vector<nlohmann::json> out;
    out.reserve(snapshot.size());
    for (const auto& [request_id, pending] : snapshot) {
        std::lock_guard<std::mutex> lk(pending->mu);
        if (pending->responded) continue;
        auto payload = pending->request_payload;
        if (!payload.contains("request_id")) {
            payload["request_id"] = request_id;
        }
        out.push_back(std::move(payload));
    }
    return out;
}

} // namespace acecode
