#include <gtest/gtest.h>

#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "session/todo_state.hpp"
#include "tool/todo_write_tool.hpp"

#include <filesystem>
#include <cstdint>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace {

fs::path make_temp_cwd(const std::string& name) {
    auto p = fs::temp_directory_path() /
        (name + "_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
         "_" + std::to_string(reinterpret_cast<std::uintptr_t>(&name)));
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

} // namespace

TEST(TodoState, ReplaceDeduplicatesByLastIdPosition) {
    nlohmann::json input = nlohmann::json::array({
        {{"id", "1"}, {"content", "first"}, {"status", "pending"}},
        {{"id", "2"}, {"content", "second"}, {"status", "pending"}},
        {{"id", "1"}, {"content", "latest"}, {"status", "in_progress"}},
    });

    auto items = acecode::replace_todo_items_from_json(input);
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0].id, "2");
    EXPECT_EQ(items[1].id, "1");
    EXPECT_EQ(items[1].content, "latest");
    EXPECT_EQ(items[1].status, "in_progress");
}

TEST(TodoState, MergeUpdatesExistingAndAppendsNewItems) {
    std::vector<acecode::TodoItem> current = {
        {"1", "original", "pending"},
    };
    nlohmann::json patch = nlohmann::json::array({
        {{"id", "1"}, {"status", "completed"}},
        {{"id", "2"}, {"content", "next"}, {"status", "pending"}},
    });

    auto items = acecode::merge_todo_items_from_json(current, patch);
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0].content, "original");
    EXPECT_EQ(items[0].status, "completed");
    EXPECT_EQ(items[1].id, "2");
    EXPECT_EQ(items[1].content, "next");
}

TEST(TodoWriteTool, WritesSessionTodosAndEmitsPayload) {
    auto cwd = make_temp_cwd("acecode_todowrite_tool");
    acecode::SessionManager sm;
    sm.start_session(cwd.string(), "stub", "stub-model", "20260603-120000-abcd");

    nlohmann::json args = {
        {"todos", nlohmann::json::array({
            {{"id", "inspect"}, {"content", "Inspect Hermes"}, {"status", "completed"}},
            {{"id", "wire"}, {"content", "Wire ACECode UI"}, {"status", "in_progress"}},
        })},
    };

    nlohmann::json emitted;
    acecode::ToolContext ctx;
    ctx.session_manager = &sm;
    ctx.emit_todo_updated = [&emitted](const nlohmann::json& payload) {
        emitted = payload;
    };

    auto tool = acecode::create_todo_write_tool();
    auto result = tool.execute(args.dump(), ctx);

    ASSERT_TRUE(result.success) << result.output;
    auto out = nlohmann::json::parse(result.output);
    EXPECT_EQ(out["summary"]["total"], 2);
    EXPECT_EQ(out["summary"]["completed"], 1);
    ASSERT_TRUE(emitted.is_object());
    EXPECT_EQ(emitted["todos"].size(), 2u);
    EXPECT_EQ(emitted["summary"]["in_progress"], 1);

    auto meta = sm.load_session_meta(sm.current_session_id());
    ASSERT_EQ(meta.todos.size(), 2u);
    EXPECT_EQ(meta.todos[1].status, "in_progress");

    fs::remove_all(cwd);
}

TEST(TodoWriteTool, MergePreservesExistingContent) {
    auto cwd = make_temp_cwd("acecode_todowrite_merge");
    acecode::SessionManager sm;
    sm.start_session(cwd.string(), "stub", "stub-model", "20260603-120001-abcd");
    sm.set_todos({{"1", "Original task", "pending"}});

    nlohmann::json args = {
        {"merge", true},
        {"todos", nlohmann::json::array({
            {{"id", "1"}, {"status", "completed"}},
        })},
    };

    acecode::ToolContext ctx;
    ctx.session_manager = &sm;
    auto result = acecode::create_todo_write_tool().execute(args.dump(), ctx);

    ASSERT_TRUE(result.success) << result.output;
    auto items = sm.current_todos();
    ASSERT_EQ(items.size(), 1u);
    EXPECT_EQ(items[0].content, "Original task");
    EXPECT_EQ(items[0].status, "completed");

    fs::remove_all(cwd);
}
