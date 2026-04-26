#pragma once

// LocalSessionClient: SessionClient 的同进程实现。所有方法直接转发到
// SessionRegistry + AgentLoop + AsyncPrompter。daemon HTTP handler 通过这个
// 客户端跟 worker 沟通。
//
// 浏览器侧的 RemoteSessionClient(走 HTTP/WebSocket 的 wire format)由
// add-web-chat-ui change 落地,接口契约相同。

#include "session_client.hpp"
#include "session_registry.hpp"

#include <memory>

namespace acecode {

class LocalSessionClient : public SessionClient {
public:
    explicit LocalSessionClient(SessionRegistry& registry) : registry_(registry) {}

    std::string create_session(const SessionOptions& opts) override;
    std::vector<SessionInfo> list_sessions() override;
    void destroy_session(const std::string& id) override;
    SubscriptionId subscribe(const std::string& session_id,
                              EventListener on_event,
                              std::uint64_t since_seq = 0) override;
    void unsubscribe(const std::string& session_id, SubscriptionId sub) override;
    void send_input(const std::string& session_id, const std::string& text) override;
    void respond_permission(const std::string& session_id,
                              const PermissionDecision& decision) override;
    void abort(const std::string& session_id) override;

private:
    SessionRegistry& registry_;
};

} // namespace acecode
