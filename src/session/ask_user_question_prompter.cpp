#include "ask_user_question_prompter.hpp"

#include "../utils/uuid.hpp"

namespace acecode {

std::string AskUserQuestionPrompter::make_request_id() {
    return generate_uuid();
}

AskUserQuestionResponse
AskUserQuestionPrompter::prompt(const nlohmann::json& questions_payload,
                                  const std::atomic<bool>* abort_flag) {
    std::string req_id = make_request_id();
    auto pending = std::make_shared<Pending>();
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        pending_[req_id] = pending;
    }

    // 推 QuestionRequest 事件;前端拿到后弹 modal,提交后通过
    // SessionClient::respond_question 回应。
    nlohmann::json payload;
    payload["request_id"] = req_id;
    payload["questions"]  = questions_payload;
    events_.emit(SessionEventKind::QuestionRequest, payload);

    auto deadline = std::chrono::steady_clock::now() + timeout_;
    AskUserQuestionResponse result;
    result.cancelled = true; // 默认值(超时 / abort 时返回)

    while (true) {
        std::unique_lock<std::mutex> lk(pending->mu);
        bool got = pending->cv.wait_for(lk, std::chrono::milliseconds(50),
            [&] { return pending->responded; });

        if (got) {
            result = pending->response;
            break;
        }
        if (abort_flag && abort_flag->load()) {
            // abort_flag 走出循环 = cancelled 默认值生效
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            events_.emit(SessionEventKind::Error, nlohmann::json{
                {"reason",     "question_timeout"},
                {"request_id", req_id},
            });
            break;
        }
    }

    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        pending_.erase(req_id);
    }
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
        p->response = response;
        p->responded = true;
    }
    p->cv.notify_all();
}

std::size_t AskUserQuestionPrompter::pending_count() const {
    std::lock_guard<std::mutex> lk(pending_mu_);
    return pending_.size();
}

} // namespace acecode
