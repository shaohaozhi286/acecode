#include <gtest/gtest.h>

#include "session/session_manager.hpp"
#include "session/session_replay.hpp"
#include "session/session_storage.hpp"
#include "session/tool_result_storage.hpp"
#include "tool/tool_executor.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path temp_dir(const std::string& hint) {
    auto dir = fs::temp_directory_path() /
        ("acecode_tool_result_storage_" + hint + "_" +
         std::to_string(std::random_device{}()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

std::string read_file(const fs::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(ifs),
                       std::istreambuf_iterator<char>());
}

acecode::ChatMessage tool_message(const std::string& id, const std::string& content) {
    acecode::ChatMessage msg;
    msg.role = "tool";
    msg.tool_call_id = id;
    msg.content = content;
    return msg;
}

} // namespace

TEST(ToolResultStorage, PersistsLargestFreshResultUntilBatchUnderBudget) {
    auto dir = temp_dir("budget");

    std::vector<acecode::ToolCall> calls(3);
    calls[0].id = "call-large";
    calls[1].id = "call-medium";
    calls[2].id = "call-small";

    std::vector<acecode::ToolResult> results = {
        acecode::ToolResult{std::string(3000, 'a'), true},
        acecode::ToolResult{std::string(500, 'b'), true},
        acecode::ToolResult{std::string(100, 'c'), true},
    };
    std::vector<bool> ready = {true, true, true};

    acecode::ToolResultReplacementState state;
    acecode::ToolResultBudgetOptions opts;
    opts.per_batch_budget_bytes = 2500;
    opts.preview_bytes = 64;

    auto budget = acecode::enforce_tool_result_budget(
        calls, results, ready, dir.string(), state, opts);

    ASSERT_EQ(budget.newly_replaced.size(), 1u);
    EXPECT_EQ(budget.newly_replaced[0].tool_call_id, "call-large");
    EXPECT_TRUE(acecode::is_persisted_output_message(results[0].output));
    EXPECT_EQ(results[1].output, std::string(500, 'b'));
    EXPECT_EQ(results[2].output, std::string(100, 'c'));

    const fs::path persisted = dir / "call-large.txt";
    ASSERT_TRUE(fs::exists(persisted));
    EXPECT_EQ(read_file(persisted), std::string(3000, 'a'));
    EXPECT_TRUE(state.seen_ids.count("call-large"));
    EXPECT_TRUE(state.replacements.count("call-large"));

    fs::remove_all(dir);
}

TEST(ToolResultStorage, MetaRecordsReconstructAndReapplyExactReplacement) {
    const std::string replacement =
        std::string(acecode::PERSISTED_OUTPUT_TAG) +
        "\nOutput too large (10B). Full output saved to: /tmp/out.txt\n\n"
        "Preview (first 2.0KB):\nabc\n" +
        acecode::PERSISTED_OUTPUT_CLOSING_TAG;

    std::vector<acecode::ToolResultReplacementRecord> records = {
        {"call-1", replacement},
    };
    acecode::ChatMessage meta = acecode::encode_content_replacement_message(records);

    std::vector<acecode::ChatMessage> messages = {
        tool_message("call-1", "full inline content"),
        meta,
    };

    auto state = acecode::reconstruct_tool_result_replacement_state(messages);
    EXPECT_TRUE(state.seen_ids.count("call-1"));
    ASSERT_TRUE(state.replacements.count("call-1"));
    EXPECT_EQ(state.replacements["call-1"], replacement);

    EXPECT_EQ(acecode::apply_tool_result_replacements(messages, state), 1);
    EXPECT_EQ(messages[0].content, replacement);
}

TEST(ToolResultStorage, SessionResumeReappliesReplacementRecords) {
    auto cwd = temp_dir("resume_cwd");
    auto project_dir = acecode::SessionStorage::get_project_dir(cwd.string());
    fs::remove_all(project_dir);

    const std::string replacement =
        std::string(acecode::PERSISTED_OUTPUT_TAG) +
        "\nOutput too large (4B). Full output saved to: /tmp/tool.txt\n\n"
        "Preview (first 2.0KB):\nprev\n" +
        acecode::PERSISTED_OUTPUT_CLOSING_TAG;

    acecode::SessionManager writer;
    writer.start_session(cwd.string(), "stub", "stub-model");
    writer.on_message(tool_message("call-resume", "full content that should be replaced"));
    writer.on_message(acecode::encode_content_replacement_message({{"call-resume", replacement}}));
    const std::string sid = writer.current_session_id();
    writer.finalize();

    acecode::SessionManager reader;
    reader.start_session(cwd.string(), "stub", "stub-model");
    auto messages = reader.resume_session(sid);
    ASSERT_GE(messages.size(), 2u);
    EXPECT_EQ(messages[0].content, replacement);

    fs::remove_all(project_dir);
    fs::remove_all(cwd);
}

TEST(ToolResultStorage, SessionArtifactPathIsNarrowlyAllowed) {
    auto cwd = temp_dir("allow_cwd");
    auto project_dir = acecode::SessionStorage::get_project_dir(cwd.string());
    fs::remove_all(project_dir);

    acecode::SessionManager sm;
    sm.start_session(cwd.string(), "stub", "stub-model", "sid-artifacts");
    const std::string dir = sm.ensure_tool_results_dir();
    ASSERT_FALSE(dir.empty());

    const fs::path artifact = fs::path(dir) / "call.txt";
    {
        std::ofstream ofs(artifact);
        ofs << "saved";
    }

    EXPECT_TRUE(sm.is_tool_result_artifact_path(artifact.string()));
    EXPECT_FALSE(sm.is_tool_result_artifact_path((cwd / "normal.txt").string()));
    EXPECT_FALSE(sm.is_tool_result_artifact_path(
        (fs::temp_directory_path() / "outside-tool-result.txt").string()));

    fs::remove_all(project_dir);
    fs::remove_all(cwd);
}

TEST(ToolResultStorage, ContentReplacementMetaIsHiddenFromReplay) {
    acecode::ToolExecutor tools;
    std::vector<acecode::ChatMessage> messages = {
        tool_message("call-1", "visible tool result"),
        acecode::encode_content_replacement_message({
            {"call-1", std::string(acecode::PERSISTED_OUTPUT_TAG) + "\npreview\n" +
                           acecode::PERSISTED_OUTPUT_CLOSING_TAG},
        }),
    };

    auto rows = acecode::replay_session_messages(messages, tools);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].role, "tool_result");
    EXPECT_EQ(rows[0].content, "visible tool result");
}
