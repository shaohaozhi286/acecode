#include "question_policy.hpp"

namespace acecode {

namespace {

QuestionPolicy parse_policy_or_ask(const std::string& value) {
    if (value == "deny") return QuestionPolicy::Deny;
    if (value == "timeout") return QuestionPolicy::Timeout;
    return QuestionPolicy::Ask;
}

int sanitize_timeout_seconds(int v) {
    if (v < 5 || v > 3600) return 60;
    return v;
}

} // namespace

ResolvedQuestionPolicy resolve_question_policy(
    const std::string& configured_policy,
    bool policy_explicit,
    int configured_timeout_seconds,
    const std::string& permission_mode) {
    ResolvedQuestionPolicy out;
    out.timeout_seconds = sanitize_timeout_seconds(configured_timeout_seconds);

    if (policy_explicit) {
        out.policy = parse_policy_or_ask(configured_policy);
        out.origin = "explicit";
        return out;
    }
    if (permission_mode == "yolo") {
        out.policy = QuestionPolicy::Deny;
        out.origin = "yolo-implicit";
        return out;
    }
    out.policy = QuestionPolicy::Ask;
    out.origin = "default";
    return out;
}

} // namespace acecode
