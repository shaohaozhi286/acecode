#include "permission_prompter.hpp"

#include "../utils/uuid.hpp"

#include <memory>
#include <utility>
#include <vector>

namespace acecode {

std::string AsyncPrompter::make_request_id() {
    return generate_uuid();
}

PermissionResult AsyncPrompter::prompt(const std::string& tool_name,
                                         const std::string& args_json,
                                         const std::atomic<bool>* abort_flag) {
    std::string req_id = make_request_id();

    // 工具参数 JSON parse 失败时仍然作为字符串透传,前端自己决定渲染。
    nlohmann::json args_payload;
    try {
        args_payload = nlohmann::json::parse(args_json);
    } catch (...) {
        args_payload = args_json;
    }

    auto pending = std::make_shared<Pending>();
    pending->tool_name = tool_name;   // 补发用:见 snapshot_pending_requests
    pending->args      = args_payload;
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        pending_[req_id] = pending;
    }

    // 推 PermissionRequest 事件到 EventDispatcher。客户端(浏览器)拿到后
    // 应弹框,然后通过 SessionClient::respond_permission 回应。
    events_.emit(SessionEventKind::PermissionRequest, nlohmann::json{
        {"request_id", req_id},
        {"tool",       tool_name},
        {"args",       args_payload},
    });

    // 阻塞等响应 / abort / 超时。abort_flag 用 50ms 节奏轮询 — 不算热循环,
    // worker thread 多 50ms 退出延迟可接受。
    auto deadline = std::chrono::steady_clock::now() + timeout_;
    PermissionDecisionChoice final_choice = PermissionDecisionChoice::Deny;
    std::string close_reason = "abort";
    bool timed_out = false;

    while (true) {
        std::unique_lock<std::mutex> lk(pending->mu);
        bool got = pending->cv.wait_for(lk, std::chrono::milliseconds(50),
            [&] { return pending->responded; });

        if (got) {
            final_choice = pending->choice;
            close_reason = pending->reason;
            break;
        }
        if (abort_flag && abort_flag->load()) {
            pending->choice = PermissionDecisionChoice::Deny;
            pending->reason = "abort";
            pending->responded = true;
            final_choice = pending->choice;
            close_reason = pending->reason;
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            pending->choice = PermissionDecisionChoice::Deny;
            pending->reason = "timeout";
            pending->responded = true;
            final_choice = pending->choice;
            close_reason = pending->reason;
            timed_out = true;
            break;
        }
    }

    // 清理 pending 表(同一 req_id 不会被复用,但 map 不能无限增长)
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        pending_.erase(req_id);
    }

    events_.emit(SessionEventKind::PermissionClosed, nlohmann::json{
        {"request_id", req_id},
        {"choice",     to_string(final_choice)},
        {"reason",     close_reason},
    });

    if (timed_out) {
        // 先发布 close 再保留既有 timeout error。后台 Web 客户端收到 error
        // 时可能释放 session 订阅,close 必须先到才能避免留下不可操作的旧卡片。
        events_.emit(SessionEventKind::Error, nlohmann::json{
            {"reason",     "permission_timeout"},
            {"request_id", req_id},
        });
    }
    return to_permission_result(final_choice);
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
        if (p->responded) return;
        p->choice = choice;
        p->reason = "decision";
        p->responded = true;
    }
    p->cv.notify_all();
}

std::vector<nlohmann::json> AsyncPrompter::snapshot_pending_requests() {
    // 加锁顺序对齐 resolve_all:先在 pending_mu_ 下把 (id, Pending) 复制出来,
    // 释放后再逐个短暂锁 p->mu 读取 —— 全程不同时持两把锁,避免与
    // prompt/notify_decision 的锁序交叉造成死锁。tool_name/args 在 prompt 入队时
    // 写定、此后只读,加 p->mu 只是为了与 responded 一致读取。
    std::vector<std::pair<std::string, std::shared_ptr<Pending>>> snapshot;
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        snapshot.reserve(pending_.size());
        for (const auto& [rid, p] : pending_) {
            if (p) snapshot.emplace_back(rid, p);
        }
    }
    std::vector<nlohmann::json> out;
    for (const auto& [rid, p] : snapshot) {
        std::lock_guard<std::mutex> plk(p->mu);
        if (p->responded) continue;
        out.push_back(nlohmann::json{
            {"request_id", rid},
            {"tool",       p->tool_name},
            {"args",       p->args},
        });
    }
    return out;
}

void AsyncPrompter::resolve_all(PermissionDecisionChoice choice) {
    std::vector<std::shared_ptr<Pending>> pending;
    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        pending.reserve(pending_.size());
        for (const auto& item : pending_) {
            pending.push_back(item.second);
        }
    }

    for (const auto& p : pending) {
        if (!p) continue;
        {
            std::lock_guard<std::mutex> lk(p->mu);
            if (p->responded) continue;
            p->choice = choice;
            p->reason = "permission_mode_change";
            p->responded = true;
        }
        p->cv.notify_all();
    }
}

} // namespace acecode
