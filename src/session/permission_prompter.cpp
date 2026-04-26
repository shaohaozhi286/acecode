#include "permission_prompter.hpp"

#include "../utils/uuid.hpp"

namespace acecode {

std::string AsyncPrompter::make_request_id() {
    return generate_uuid();
}

PermissionResult AsyncPrompter::prompt(const std::string& tool_name,
                                         const std::string& args_json,
                                         const std::atomic<bool>* abort_flag) {
    std::string req_id = make_request_id();
    auto pending = std::make_shared<Pending>();
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        pending_[req_id] = pending;
    }

    // 推 PermissionRequest 事件到 EventDispatcher。客户端(浏览器)拿到后
    // 应弹框,然后通过 SessionClient::respond_permission 回应。
    nlohmann::json args_payload;
    try {
        args_payload = nlohmann::json::parse(args_json);
    } catch (...) {
        // 工具参数 JSON parse 失败时仍然作为字符串透传,前端自己决定渲染。
        args_payload = args_json;
    }
    events_.emit(SessionEventKind::PermissionRequest, nlohmann::json{
        {"request_id", req_id},
        {"tool",       tool_name},
        {"args",       args_payload},
    });

    // 阻塞等响应 / abort / 超时。abort_flag 用 50ms 节奏轮询 — 不算热循环,
    // worker thread 多 50ms 退出延迟可接受。
    auto deadline = std::chrono::steady_clock::now() + timeout_;
    PermissionResult result = PermissionResult::Deny;

    while (true) {
        std::unique_lock<std::mutex> lk(pending->mu);
        bool got = pending->cv.wait_for(lk, std::chrono::milliseconds(50),
            [&] { return pending->responded; });

        if (got) {
            result = to_permission_result(pending->choice);
            break;
        }
        if (abort_flag && abort_flag->load()) {
            break; // Deny
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            // 超时: spec 要求推 error 事件 + 视为 deny
            events_.emit(SessionEventKind::Error, nlohmann::json{
                {"reason",     "permission_timeout"},
                {"request_id", req_id},
            });
            break;
        }
    }

    // 清理 pending 表(同一 req_id 不会被复用,但 map 不能无限增长)
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        pending_.erase(req_id);
    }
    return result;
}

void AsyncPrompter::notify_decision(const std::string& request_id,
                                       PermissionDecisionChoice choice) {
    std::shared_ptr<Pending> p;
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        auto it = pending_.find(request_id);
        if (it == pending_.end()) return; // 未知 / 已超时
        p = it->second;
    }
    {
        std::lock_guard<std::mutex> lk(p->mu);
        p->choice = choice;
        p->responded = true;
    }
    p->cv.notify_all();
}

} // namespace acecode
