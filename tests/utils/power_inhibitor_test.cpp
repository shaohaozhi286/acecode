#include "utils/power_inhibitor.hpp"

#include <gtest/gtest.h>

namespace {

struct FakeBackend {
    int acquires = 0;
    int releases = 0;
    bool acquire_ok = true;
};

acecode::ActiveSessionPowerGuard make_guard(FakeBackend& backend) {
    return acecode::ActiveSessionPowerGuard(
        [&backend](std::string* error) {
            backend.acquires += 1;
            if (!backend.acquire_ok) {
                if (error) *error = "fake failure";
                return false;
            }
            return true;
        },
        [&backend] {
            backend.releases += 1;
        });
}

} // namespace

TEST(ActiveSessionPowerGuard, FirstBusyAcquiresAndLastIdleReleases) {
    FakeBackend backend;
    auto guard = make_guard(backend);

    guard.set_busy("a", true);
    EXPECT_EQ(backend.acquires, 1);
    EXPECT_EQ(backend.releases, 0);
    EXPECT_TRUE(guard.inhibitor_active());
    EXPECT_EQ(guard.busy_count(), 1u);

    guard.set_busy("b", true);
    EXPECT_EQ(backend.acquires, 1);
    EXPECT_EQ(guard.busy_count(), 2u);

    guard.set_busy("a", false);
    EXPECT_EQ(backend.releases, 0);
    EXPECT_TRUE(guard.inhibitor_active());

    guard.set_busy("b", false);
    EXPECT_EQ(backend.releases, 1);
    EXPECT_FALSE(guard.inhibitor_active());
    EXPECT_EQ(guard.busy_count(), 0u);
}

TEST(ActiveSessionPowerGuard, DuplicateBusyAndIdleAreIdempotent) {
    FakeBackend backend;
    auto guard = make_guard(backend);

    guard.set_busy("a", true);
    guard.set_busy("a", true);
    EXPECT_EQ(backend.acquires, 1);
    EXPECT_EQ(guard.busy_count(), 1u);

    guard.set_busy("missing", false);
    EXPECT_EQ(backend.releases, 0);
    EXPECT_TRUE(guard.inhibitor_active());

    guard.set_busy("a", false);
    guard.set_busy("a", false);
    EXPECT_EQ(backend.releases, 1);
    EXPECT_FALSE(guard.inhibitor_active());
}

TEST(ActiveSessionPowerGuard, ReleaseAllClearsBusySessions) {
    FakeBackend backend;
    auto guard = make_guard(backend);

    guard.set_busy("a", true);
    guard.set_busy("b", true);
    guard.release_all();

    EXPECT_EQ(backend.acquires, 1);
    EXPECT_EQ(backend.releases, 1);
    EXPECT_EQ(guard.busy_count(), 0u);
    EXPECT_FALSE(guard.inhibitor_active());
}

TEST(ActiveSessionPowerGuard, FailedAcquireDoesNotRetryUntilNextEpoch) {
    FakeBackend backend;
    backend.acquire_ok = false;
    auto guard = make_guard(backend);

    guard.set_busy("a", true);
    EXPECT_EQ(backend.acquires, 1);
    EXPECT_FALSE(guard.inhibitor_active());
    EXPECT_EQ(guard.last_error(), "fake failure");

    guard.set_busy("b", true);
    EXPECT_EQ(backend.acquires, 1);

    guard.set_busy("a", false);
    guard.set_busy("b", false);
    EXPECT_EQ(backend.releases, 0);

    backend.acquire_ok = true;
    guard.set_busy("c", true);
    EXPECT_EQ(backend.acquires, 2);
    EXPECT_TRUE(guard.inhibitor_active());
}
