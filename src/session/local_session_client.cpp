#include "local_session_client.hpp"

#include "ask_user_question_prompter.hpp"
#include "session_storage.hpp"
#include "../utils/logger.hpp"

namespace acecode {

std::string LocalSessionClient::create_session(const SessionOptions& opts) {
    return registry_.create(opts);
}

bool LocalSessionClient::resume_session(const std::string& id) {
    return registry_.resume(id);
}

std::vector<SessionInfo> LocalSessionClient::list_sessions() {
    // v1 简化: 只返回内存活跃的。磁盘历史由 HTTP /api/sessions 单独的
    // SessionStorage::list_sessions 路径合并(后续 Section 9)。
    return registry_.list_active();
}

void LocalSessionClient::destroy_session(const std::string& id) {
    registry_.destroy(id);
}

LocalSessionClient::SubscriptionId
LocalSessionClient::subscribe(const std::string& session_id,
                                EventListener on_event,
                                std::uint64_t since_seq) {
    SessionEntry* entry = registry_.lookup(session_id);
    if (!entry || !entry->loop) return 0;
    return entry->loop->events().subscribe(std::move(on_event), since_seq);
}

void LocalSessionClient::unsubscribe(const std::string& session_id, SubscriptionId sub) {
    SessionEntry* entry = registry_.lookup(session_id);
    if (!entry || !entry->loop) return;
    entry->loop->events().unsubscribe(sub);
}

bool LocalSessionClient::send_input(const std::string& session_id, const std::string& text) {
    SessionEntry* entry = registry_.lookup(session_id);
    if (!entry || !entry->loop) {
        LOG_WARN("[client] send_input on unknown session " + session_id);
        return false;
    }
    entry->loop->submit(text);
    return true;
}

void LocalSessionClient::respond_permission(const std::string& session_id,
                                               const PermissionDecision& decision) {
    SessionEntry* entry = registry_.lookup(session_id);
    if (!entry || !entry->prompter) {
        LOG_WARN("[client] respond_permission on unknown session " + session_id);
        return;
    }
    entry->prompter->notify_decision(decision.request_id, decision.choice);
}

void LocalSessionClient::respond_question(const std::string& session_id,
                                              const std::string& request_id,
                                              const AskUserQuestionResponse& response) {
    SessionEntry* entry = registry_.lookup(session_id);
    if (!entry || !entry->ask_prompter) {
        LOG_WARN("[client] respond_question on unknown session " + session_id);
        return;
    }
    entry->ask_prompter->notify_response(request_id, response);
}

void LocalSessionClient::abort(const std::string& session_id) {
    SessionEntry* entry = registry_.lookup(session_id);
    if (!entry || !entry->loop) return;
    entry->loop->abort();
}

} // namespace acecode
