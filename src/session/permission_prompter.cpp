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
            p->responded = true;
        }
        p->cv.notify_all();
    }
}

} // namespace acecode
