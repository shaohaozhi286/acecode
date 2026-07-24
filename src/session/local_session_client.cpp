#include "local_session_client.hpp"

#include "ask_user_question_prompter.hpp"
#include "session_storage.hpp"
#include "../utils/logger.hpp"

namespace acecode {

std::string LocalSessionClient::create_session(const SessionOptions& opts) {
    return registry_.create(opts);
}

bool LocalSessionClient::resume_session(const std::string& id, const SessionOptions& opts) {
    return registry_.resume(id, opts);
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
    auto entry = registry_.acquire(session_id);
    if (!entry || !entry->loop) return 0;
    return entry->loop->events().subscribe(std::move(on_event), since_seq);
}

void LocalSessionClient::unsubscribe(const std::string& session_id, SubscriptionId sub) {
    auto entry = registry_.acquire(session_id);
    if (!entry || !entry->loop) return;
    entry->loop->events().unsubscribe(sub);
}

bool LocalSessionClient::send_input(const std::string& session_id, const std::string& text) {
    return send_input(session_id, text, std::string{});
}

bool LocalSessionClient::send_input(const std::string& session_id,
                                       const std::string& text,
                                       const std::string& display_text) {
    UserInput input;
    input.text = text;
    input.display_text = display_text;
    return send_input(session_id, input);
}

bool LocalSessionClient::send_input(const std::string& session_id, const UserInput& input) {
    auto entry = registry_.acquire(session_id);
    if (!entry || !entry->loop) {
        LOG_WARN("[client] send_input on unknown session " + session_id);
        return false;
    }
    registry_.maybe_start_auto_title(session_id, input);
    entry->loop->submit(input);
    return true;
}

TurnSteerResult LocalSessionClient::steer_input(
    const std::string& session_id,
    const std::string& expected_turn_id,
    const UserInput& input) {
    auto entry = registry_.acquire(session_id);
    if (!entry || !entry->loop) {
        LOG_WARN("[client] steer_input on unknown session " + session_id);
        return {
            TurnSteerStatus::UnknownSession,
            {},
            "unknown session",
        };
    }
    return entry->loop->steer_input(expected_turn_id, input);
}

BuiltinCommandResult LocalSessionClient::execute_builtin_command(
    const std::string& session_id,
    const BuiltinCommandRequest& request) {
    return registry_.execute_builtin_command(session_id, request);
}

void LocalSessionClient::respond_permission(const std::string& session_id,
                                               const PermissionDecision& decision) {
    auto entry = registry_.acquire(session_id);
    if (!entry || !entry->prompter) {
        LOG_WARN("[client] respond_permission on unknown session " + session_id);
        return;
    }
    entry->prompter->notify_decision(decision.request_id, decision.choice);
}

QuestionResponseStatus LocalSessionClient::respond_question(
    const std::string& session_id,
    const std::string& request_id,
    const AskUserQuestionResponse& response) {
    auto entry = registry_.acquire(session_id);
    if (!entry || !entry->ask_prompter) {
        LOG_WARN("[client] respond_question on unknown session " + session_id);
        return QuestionResponseStatus::UnknownSession;
    }
    return entry->ask_prompter->notify_response(request_id, response)
               ? QuestionResponseStatus::Accepted
               : QuestionResponseStatus::Closed;
}

std::optional<std::vector<PendingQuestionRequestSnapshot>>
LocalSessionClient::snapshot_pending_questions(const std::string& session_id) {
    auto entry = registry_.acquire(session_id);
    if (!entry || !entry->ask_prompter) return std::nullopt;
    return entry->ask_prompter->snapshot_pending_question_requests();
}

void LocalSessionClient::abort(const std::string& session_id) {
    auto entry = registry_.acquire(session_id);
    if (!entry || !entry->loop) return;
    entry->loop->abort();
}

} // namespace acecode
