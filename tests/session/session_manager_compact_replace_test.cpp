#include <gtest/gtest.h>

#include "session/file_checkpoint_store.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"

#include <filesystem>
#include <random>

namespace {

acecode::ChatMessage make_message(std::string role, std::string content, std::string uuid = {}) {
    acecode::ChatMessage msg;
    msg.role = std::move(role);
    msg.content = std::move(content);
    msg.uuid = std::move(uuid);
    return msg;
}

std::filesystem::path make_temp_cwd() {
    auto dir = std::filesystem::temp_directory_path() /
        ("acecode_session_replace_" + std::to_string(std::random_device{}()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

} // namespace

TEST(SessionManagerCompactReplace, RewritesJsonlAndKeepsRetainedCheckpointsOnly) {
    auto cwd = make_temp_cwd();
    const std::string project_dir = acecode::SessionStorage::get_project_dir(cwd.string());
    std::filesystem::remove_all(project_dir);

    acecode::SessionManager sm;
    const std::string session_id = acecode::SessionStorage::generate_session_id();
    sm.start_session(cwd.string(), "stub", "stub-model", session_id);

    auto old_user = make_message("user", "old prompt", "u-old");
    auto kept_user = make_message("user", "kept prompt", "u-kept");
    auto kept_assistant = make_message("assistant", "kept response");

    sm.on_message(old_user);
    sm.begin_user_turn_checkpoint(old_user.uuid);
    sm.on_message(make_message("assistant", "old response"));
    sm.on_message(kept_user);
    sm.begin_user_turn_checkpoint(kept_user.uuid);
    sm.on_message(kept_assistant);

    acecode::ChatMessage boundary;
    boundary.role = "system";
    boundary.subtype = "compact_boundary";
    boundary.is_meta = true;

    acecode::ChatMessage summary;
    summary.role = "system";
    summary.content = "[Conversation summary]\nold prompt summarized";
    summary.is_compact_summary = true;

    ASSERT_TRUE(sm.replace_active_messages({boundary, summary, kept_user, kept_assistant}));

    auto candidates = acecode::SessionStorage::find_session_files(project_dir, session_id);
    ASSERT_FALSE(candidates.empty());
    auto stored = acecode::SessionStorage::load_messages(candidates.front().jsonl_path);

    bool saw_old_user = false;
    bool saw_kept_user = false;
    int checkpoint_count = 0;
    std::string checkpoint_user;
    for (const auto& msg : stored) {
        if (msg.content == "old prompt") saw_old_user = true;
        if (msg.content == "kept prompt") saw_kept_user = true;
        auto snapshot = acecode::FileCheckpointStore::decode_snapshot_message(msg);
        if (snapshot.has_value()) {
            checkpoint_count++;
            checkpoint_user = snapshot->message_uuid;
        }
    }

    EXPECT_FALSE(saw_old_user);
    EXPECT_TRUE(saw_kept_user);
    EXPECT_EQ(checkpoint_count, 1);
    EXPECT_EQ(checkpoint_user, "u-kept");

    auto meta = acecode::SessionStorage::read_meta(candidates.front().meta_path);
    EXPECT_EQ(meta.message_count, static_cast<int>(stored.size()));
    EXPECT_EQ(meta.summary, "kept prompt");

    std::error_code ec;
    std::filesystem::remove_all(project_dir, ec);
    std::filesystem::remove_all(cwd, ec);
}
