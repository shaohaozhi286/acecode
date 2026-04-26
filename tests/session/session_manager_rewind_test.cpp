#include <gtest/gtest.h>

#include "provider/llm_provider.hpp"
#include "session/session_manager.hpp"
#include "session/session_rewind.hpp"
#include "session/session_storage.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using acecode::ChatMessage;
using acecode::SessionManager;
using acecode::SessionStorage;

namespace {

fs::path make_temp_cwd(const std::string& hint) {
    auto dir = fs::temp_directory_path() /
               ("acecode_session_rewind_" + hint + "_" +
                std::to_string(std::random_device{}()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream ofs(path, std::ios::binary);
    ofs << content;
}

std::string read_file(const fs::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(ifs)),
                       std::istreambuf_iterator<char>());
}

ChatMessage user_msg(const std::string& uuid, const std::string& content) {
    ChatMessage msg;
    msg.role = "user";
    msg.uuid = uuid;
    msg.timestamp = "2026-04-26T00:00:00Z";
    msg.content = content;
    return msg;
}

bool contains_content(const std::vector<ChatMessage>& messages, const std::string& content) {
    for (const auto& msg : messages) {
        if (msg.content == content) return true;
    }
    return false;
}

} // namespace

TEST(SessionManagerRewind, ForkWritesTrimmedSessionAndKeepsOriginalListed) {
    auto cwd = make_temp_cwd("fork");
    auto project_dir = SessionStorage::get_project_dir(cwd.string());
    fs::remove_all(project_dir);

    SessionManager sm;
    sm.start_session(cwd.string(), "test-provider", "test-model");

    ChatMessage first = user_msg("u1", "first prompt");
    sm.on_message(first);
    sm.begin_user_turn_checkpoint("u1");

    ChatMessage tail = user_msg("u2", "discarded tail");
    sm.on_message(tail);

    const std::string old_id = sm.current_session_id();
    ASSERT_FALSE(old_id.empty());

    std::string new_id = sm.fork_active_session({first});
    ASSERT_FALSE(new_id.empty());
    EXPECT_NE(new_id, old_id);

    auto sessions = sm.list_sessions();
    bool saw_old = false;
    bool saw_new = false;
    for (const auto& meta : sessions) {
        if (meta.id == old_id) saw_old = true;
        if (meta.id == new_id) saw_new = true;
    }
    EXPECT_TRUE(saw_old);
    EXPECT_TRUE(saw_new);

    auto new_messages = SessionStorage::load_messages(
        SessionStorage::session_path(project_dir, new_id));
    EXPECT_TRUE(contains_content(new_messages, "first prompt"));
    EXPECT_FALSE(contains_content(new_messages, "discarded tail"));

    auto resumed = sm.resume_session(new_id);
    EXPECT_TRUE(contains_content(resumed, "first prompt"));
    EXPECT_FALSE(contains_content(resumed, "discarded tail"));

    fs::remove_all(project_dir);
    fs::remove_all(cwd);
}

TEST(SessionManagerRewind, ResumeReconstructsCheckpointState) {
    auto cwd = make_temp_cwd("resume");
    auto project_dir = SessionStorage::get_project_dir(cwd.string());
    fs::remove_all(project_dir);
    auto file = cwd / "tracked.txt";
    write_file(file, "old\n");

    SessionManager sm;
    sm.start_session(cwd.string(), "test-provider", "test-model");

    ChatMessage first = user_msg("u1", "edit file");
    sm.on_message(first);
    sm.begin_user_turn_checkpoint("u1");
    sm.track_file_write_before(file.string());
    write_file(file, "new\n");

    const std::string session_id = sm.current_session_id();
    ASSERT_FALSE(session_id.empty());

    auto messages = sm.resume_session(session_id);
    ASSERT_FALSE(messages.empty());

    write_file(file, "latest\n");
    auto restored = sm.rewind_files_to_checkpoint("u1");

    EXPECT_TRUE(restored.ok());
    EXPECT_EQ(read_file(file), "old\n");

    fs::remove_all(project_dir);
    fs::remove_all(cwd);
}
