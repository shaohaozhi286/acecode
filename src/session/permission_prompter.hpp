#pragma once

// PermissionPrompter: 把"问用户能不能执行这个工具"的同步阻塞决策抽出来。
// 老的 AgentCallbacks::on_tool_confirm 是个同步 lambda,TUI 弹框 → 阻塞返回
// PermissionResult。daemon 路径下"问用户"要走 WebSocket → 浏览器,响应可能
// 来自任意线程,需要 condvar + request_id map。
//
// 两个实现:
//   - CallbackPrompter: 包装老 on_tool_confirm callback。零改动 TUI。
//   - AsyncPrompter:   推 SessionEvent::PermissionRequest 到 EventDispatcher,
//                       condvar 等 respond(request_id, choice)。5 分钟超时
//                       视为 Deny。
//
// AgentLoop 持有 unique_ptr<PermissionPrompter>,默认是 CallbackPrompter
// (兼容现有 main.cpp 路径);daemon 模式由 SessionRegistry 在创建 AgentLoop
// 后调 set_permission_prompter() 注入 AsyncPrompter。

#include "../permissions.hpp"
#include "event_dispatcher.hpp"
#include "session_client.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace acecode {

class PermissionPrompter {
public:
    virtual ~PermissionPrompter() = default;
    // 阻塞调用,返回用户决定。abort_flag 非空时,prompter 应当周期性检查,
    // 在 abort 触发后立刻返回 Deny(避免 worker thread 卡 5 分钟无法 join)。
    virtual PermissionResult prompt(const std::string& tool_name,
                                      const std::string& args_json,
                                      const std::atomic<bool>* abort_flag = nullptr) = 0;
};

// 默认实现: 直接转发到 AgentCallbacks::on_tool_confirm 风格的 sync callback。
// AgentLoop 构造时如果没装 prompter,内部自动用这个包 callbacks_.on_tool_confirm。
class CallbackPrompter : public PermissionPrompter {
public:
    using SyncCallback = std::function<PermissionResult(const std::string& tool,
                                                          const std::string& args)>;
    explicit CallbackPrompter(SyncCallback cb) : cb_(std::move(cb)) {}

    PermissionResult prompt(const std::string& tool_name,
                              const std::string& args_json,
                              const std::atomic<bool>* /*abort_flag*/) override {
        if (!cb_) return PermissionResult::Deny;
        return cb_(tool_name, args_json);
    }

private:
    SyncCallback cb_;
};

// 异步实现: daemon 模式用。push 一个 PermissionRequest 事件到 EventDispatcher
// (浏览器拿到后弹框),然后 condvar 阻塞等 respond(request_id, choice) 到来。
//
// 多并发 prompt 安全: 每次 prompt 生成新的 request_id,响应通过 id 路由到
// 正确的 promise。一个 worker thread 同一时刻只会 prompt 一次(AgentLoop 串行
// 执行工具),但保留 map 设计是为了后续多 worker / 并发工具决策。
class AsyncPrompter : public PermissionPrompter {
public:
    explicit AsyncPrompter(EventDispatcher& events,
                            std::chrono::milliseconds timeout = std::chrono::minutes(5))
        : events_(events), timeout_(timeout) {}

    PermissionResult prompt(const std::string& tool_name,
                              const std::string& args_json,
                              const std::atomic<bool>* abort_flag) override;

    // 由 SessionClient::respond_permission 间接调用。线程安全。
    // 未知 request_id = no-op(可能已超时被 GC)。
    void notify_decision(const std::string& request_id, PermissionDecisionChoice choice);

private:
    static std::string make_request_id();

    EventDispatcher&         events_;
    std::chrono::milliseconds timeout_;

    struct Pending {
        std::mutex                  mu;
        std::condition_variable     cv;
        bool                        responded = false;
        PermissionDecisionChoice    choice = PermissionDecisionChoice::Deny;
    };

    std::mutex                                                pending_mu_;
    std::unordered_map<std::string, std::shared_ptr<Pending>> pending_;
};

// helper: PermissionDecisionChoice → 老 PermissionResult
inline PermissionResult to_permission_result(PermissionDecisionChoice c) {
    switch (c) {
        case PermissionDecisionChoice::Allow:        return PermissionResult::Allow;
        case PermissionDecisionChoice::Deny:         return PermissionResult::Deny;
        case PermissionDecisionChoice::AllowSession: return PermissionResult::AlwaysAllow;
    }
    return PermissionResult::Deny;
}

} // namespace acecode
