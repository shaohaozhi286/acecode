#include "web/session_status_routing.hpp"

#include <gtest/gtest.h>

#include <unordered_set>

namespace acecode::web {
namespace {

TEST(SessionStatusRouting, DirectSessionSubscriptionReceivesStatus) {
    const std::unordered_set<std::string> subscriptions{"child-1"};
    EXPECT_TRUE(session_status_matches_subscriptions(
        subscriptions, "child-1", "parent-1"));
}

TEST(SessionStatusRouting, ParentSubscriptionReceivesOwnedChildStatus) {
    const std::unordered_set<std::string> subscriptions{"parent-1"};
    EXPECT_TRUE(session_status_matches_subscriptions(
        subscriptions, "child-1", "parent-1"));
}

TEST(SessionStatusRouting, UnrelatedParentDoesNotReceiveChildStatus) {
    const std::unordered_set<std::string> subscriptions{"parent-2"};
    EXPECT_FALSE(session_status_matches_subscriptions(
        subscriptions, "child-1", "parent-1"));
}

TEST(SessionStatusRouting, EmptyOwnershipDoesNotBroadenSubscription) {
    const std::unordered_set<std::string> subscriptions{"parent-1"};
    EXPECT_FALSE(session_status_matches_subscriptions(
        subscriptions, "child-1", ""));
}

} // namespace
} // namespace acecode::web
