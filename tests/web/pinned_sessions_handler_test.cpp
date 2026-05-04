#include <gtest/gtest.h>

#include "web/handlers/pinned_sessions_handler.hpp"

#include <filesystem>
#include <fstream>
#include <cstdint>
#include <string>

namespace fs = std::filesystem;

using acecode::web::PinnedSessionsState;
using acecode::web::normalize_pinned_session_ids;
using acecode::web::pin_session_id;
using acecode::web::prune_pinned_session_ids;
using acecode::web::read_pinned_sessions_state;
using acecode::web::unpin_session_id;
using acecode::web::write_pinned_sessions_state;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() /
               ("acecode_pins_test_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

} // namespace

TEST(PinnedSessionsHandler, NormalizesEmptyAndDuplicateIds) {
    auto ids = normalize_pinned_session_ids({"a", "", "b", "a", "c", "b"});
    EXPECT_EQ(ids, (std::vector<std::string>{"a", "b", "c"}));
}

TEST(PinnedSessionsHandler, PinMovesSessionToFront) {
    auto ids = pin_session_id({"a", "b", "c"}, "b");
    EXPECT_EQ(ids, (std::vector<std::string>{"b", "a", "c"}));

    auto next = pin_session_id(ids, "d");
    EXPECT_EQ(next, (std::vector<std::string>{"d", "b", "a", "c"}));
}

TEST(PinnedSessionsHandler, UnpinRemovesSession) {
    auto ids = unpin_session_id({"a", "b", "c"}, "b");
    EXPECT_EQ(ids, (std::vector<std::string>{"a", "c"}));
}

TEST(PinnedSessionsHandler, PruneKeepsOnlyAvailableSessions) {
    auto ids = prune_pinned_session_ids({"a", "b", "c", "a"}, {"c", "a"});
    EXPECT_EQ(ids, (std::vector<std::string>{"a", "c"}));
}

TEST(PinnedSessionsHandler, MissingOrMalformedFileReadsAsEmpty) {
    TempDir tmp;
    auto missing = read_pinned_sessions_state(tmp.path / "missing.json");
    EXPECT_TRUE(missing.session_ids.empty());

    auto malformed_path = tmp.path / "bad.json";
    std::ofstream(malformed_path) << "{not json";
    auto malformed = read_pinned_sessions_state(malformed_path);
    EXPECT_TRUE(malformed.session_ids.empty());
}

TEST(PinnedSessionsHandler, WritesAndReadsState) {
    TempDir tmp;
    auto path = tmp.path / "pins" / "pinned_sessions.json";
    std::string error;
    ASSERT_TRUE(write_pinned_sessions_state(path, PinnedSessionsState{{"a", "b", "a"}}, &error)) << error;

    auto state = read_pinned_sessions_state(path);
    EXPECT_EQ(state.session_ids, (std::vector<std::string>{"a", "b"}));
}
