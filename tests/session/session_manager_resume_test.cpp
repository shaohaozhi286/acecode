#include <gtest/gtest.h>

#include "provider/llm_provider.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "session/session_usage_ledger.hpp"

#include <filesystem>
#include <fstream>
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
               ("acecode_session_resume_" + hint + "_" +
                std::to_string(std::random_device{}()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

std::size_t count_non_empty_lines(const fs::path& path) {
    std::ifstream ifs(path);
    std::size_t count = 0;
    std::string line;
    while (std::getline(ifs, line)) {
        if (!line.empty()) ++count;
    }
    return count;
}

ChatMessage message(const std::string& role, const std::string& content) {
    ChatMessage msg;
    msg.role = role;
    msg.content = content;
    return msg;
}

} // namespace

TEST(SessionManagerResume, RepeatedResumeDoesNotDuplicateCanonicalJsonl) {
    auto cwd = make_temp_cwd("canonical");
    auto project_dir = SessionStorage::get_project_dir(cwd.string());
    fs::remove_all(project_dir);

    SessionManager sm;
    sm.start_session(cwd.string(), "test-provider", "test-model");
    sm.on_message(message("user", "first"));
    sm.on_message(message("assistant", "second"));

    const std::string session_id = sm.current_session_id();
    ASSERT_FALSE(session_id.empty());

    const fs::path jsonl_path = SessionStorage::session_path(project_dir, session_id);
    ASSERT_EQ(count_non_empty_lines(jsonl_path), 2u);

    auto resumed_once = sm.resume_session(session_id);
    ASSERT_EQ(resumed_once.size(), 2u);
    EXPECT_EQ(count_non_empty_lines(jsonl_path), 2u)
        << "resume canonical 文件时不能把历史追加到自己尾部";

    auto resumed_twice = sm.resume_session(session_id);
    ASSERT_EQ(resumed_twice.size(), 2u);
    EXPECT_EQ(count_non_empty_lines(jsonl_path), 2u)
        << "重复 resume 同一 session 不应让 jsonl 行数翻倍";

    fs::remove_all(project_dir);
    fs::remove_all(cwd);
}

TEST(SessionManagerResume, PidOnlyOldDataIsRejectedAndDoesNotCreateCanonical) {
    auto cwd = make_temp_cwd("old_pid_only");
    auto project_dir = SessionStorage::get_project_dir(cwd.string());
    fs::remove_all(project_dir);
    fs::create_directories(project_dir);

    const std::string session_id = "20260426-100000-abcd";
    {
        std::ofstream ofs(SessionStorage::session_path(project_dir, session_id, 1234));
        ofs << "{\"role\":\"user\",\"content\":\"old\"}\n";
    }

    SessionManager sm;
    sm.start_session(cwd.string(), "test-provider", "test-model");

    EXPECT_FALSE(sm.has_session_file(session_id));
    EXPECT_TRUE(sm.has_incompatible_session_data(session_id));
    auto messages = sm.resume_session(session_id);
    EXPECT_TRUE(messages.empty());
    EXPECT_FALSE(fs::exists(SessionStorage::session_path(project_dir, session_id)))
        << "rejecting old PID data must not synthesize a canonical transcript";

    fs::remove_all(project_dir);
    fs::remove_all(cwd);
}

TEST(SessionManagerResume, RestoresPersistedRuntimeState) {
    auto cwd = make_temp_cwd("runtime_state");
    auto project_dir = SessionStorage::get_project_dir(cwd.string());
    fs::remove_all(project_dir);

    SessionManager writer;
    writer.start_session(cwd.string(), "test-provider", "test-model");
    writer.set_permission_mode("yolo");
    writer.on_message(message("user", "first turn"));
    writer.on_message(message("assistant", "reply"));

    acecode::TokenUsage usage;
    usage.prompt_tokens = 8000;
    usage.completion_tokens = 1200;
    usage.total_tokens = 9200;
    usage.has_data = true;
    writer.record_token_usage(usage);
    const std::string session_id = writer.current_session_id();
    writer.finalize();

    SessionManager reader;
    reader.start_session(cwd.string(), "test-provider", "test-model");
    auto messages = reader.resume_session(session_id);
    ASSERT_EQ(messages.size(), 2u);

    EXPECT_EQ(reader.current_permission_mode(), "yolo");
    EXPECT_EQ(reader.current_turn_count(), 1);
    EXPECT_EQ(reader.current_last_token_usage().prompt_tokens, 8000);
    EXPECT_EQ(reader.current_session_token_usage().total_tokens, 9200);

    auto usage_records = acecode::load_usage_ledger_records(project_dir);
    ASSERT_EQ(usage_records.size(), 1u);
    EXPECT_EQ(usage_records[0].session_id, session_id);
    EXPECT_EQ(usage_records[0].provider, "test-provider");
    EXPECT_EQ(usage_records[0].model, "test-model");
    EXPECT_EQ(usage_records[0].usage.prompt_tokens, 8000);
    EXPECT_EQ(usage_records[0].usage.total_tokens, 9200);
    EXPECT_TRUE(usage_records[0].usage.has_data);

    fs::remove_all(project_dir);
    fs::remove_all(cwd);
}

TEST(SessionManagerResume, RestoresPlanRuntimeStateAndPlanFile) {
    auto cwd = make_temp_cwd("plan_runtime_state");
    auto project_dir = SessionStorage::get_project_dir(cwd.string());
    fs::remove_all(project_dir);

    SessionManager writer;
    writer.start_session(cwd.string(), "test-provider", "test-model", "sid-plan-runtime");
    writer.set_permission_mode("plan");
    writer.set_pre_plan_permission_mode("accept-edits");
    const std::string plan_path = writer.ensure_plan_file_path();
    ASSERT_FALSE(plan_path.empty());

    std::string error;
    ASSERT_TRUE(writer.write_plan_file("1. Inspect\n2. Implement\n", &error)) << error;
    writer.on_message(message("user", "first turn"));
    const std::string session_id = writer.current_session_id();
    writer.finalize();

    auto meta = SessionStorage::read_meta(SessionStorage::meta_path(project_dir, session_id));
    EXPECT_EQ(meta.permission_mode, "plan");
    EXPECT_EQ(meta.pre_plan_permission_mode, "accept-edits");

    SessionManager reader;
    reader.start_session(cwd.string(), "test-provider", "test-model");
    auto messages = reader.resume_session(session_id);
    ASSERT_EQ(messages.size(), 1u);

    EXPECT_EQ(reader.current_permission_mode(), "plan");
    EXPECT_EQ(reader.current_pre_plan_permission_mode(), "accept-edits");
    EXPECT_EQ(reader.current_plan_file_path(), plan_path);
    EXPECT_EQ(reader.read_plan_file(), "1. Inspect\n2. Implement\n");
    EXPECT_TRUE(reader.is_plan_file_path(plan_path));
    EXPECT_FALSE(reader.is_plan_file_path((cwd / "other.md").string()));

    fs::remove_all(project_dir);
    fs::remove_all(cwd);
}

TEST(SessionManagerResume, InputDraftSurvivesMetadataRewrite) {
    auto cwd = make_temp_cwd("input_draft");
    auto project_dir = SessionStorage::get_project_dir(cwd.string());
    fs::remove_all(project_dir);

    SessionManager sm;
    sm.start_session(cwd.string(), "test-provider", "test-model");
    sm.on_message(message("user", "first turn"));
    const std::string session_id = sm.current_session_id();
    ASSERT_FALSE(session_id.empty());

    sm.set_input_draft("continue this refactor");
    auto meta = SessionStorage::read_meta(SessionStorage::meta_path(project_dir, session_id));
    EXPECT_EQ(meta.input_draft, "continue this refactor");

    sm.on_message(message("assistant", "reply"));
    meta = SessionStorage::read_meta(SessionStorage::meta_path(project_dir, session_id));
    EXPECT_EQ(meta.input_draft, "continue this refactor")
        << "regular metadata updates must preserve the active input draft";

    sm.set_input_draft("");
    meta = SessionStorage::read_meta(SessionStorage::meta_path(project_dir, session_id));
    EXPECT_TRUE(meta.input_draft.empty());

    fs::remove_all(project_dir);
    fs::remove_all(cwd);
}
