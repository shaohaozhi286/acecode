#pragma once

#include <string>
#include <unordered_set>

namespace acecode::web {

// A direct session subscription receives its own status. A parent subscription
// additionally receives status for explicitly owned child sessions so child
// discovery does not depend on workspace-wide broadcasts.
inline bool session_status_matches_subscriptions(
    const std::unordered_set<std::string>& session_subscriptions,
    const std::string& session_id,
    const std::string& parent_session_id) {
    const bool direct =
        !session_id.empty() && session_subscriptions.count(session_id) > 0;
    const bool through_parent =
        !parent_session_id.empty() &&
        session_subscriptions.count(parent_session_id) > 0;
    return direct || through_parent;
}

} // namespace acecode::web
