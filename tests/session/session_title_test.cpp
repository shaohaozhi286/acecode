#include <gtest/gtest.h>

#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "session/session_title_generator.hpp"

#include <filesystem>
#include <random>

namespace {

std::filesystem::path temp_cwd(const std::string& hint) {
    auto dir = std::filesystem::temp_directory_path() /
        ("acecode_session_title_" + hint + "_" + std::to_string(std::random_device{}()));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

struct ProjectCleanup {
    explicit ProjectCleanup(std::string cwd)
        : project_dir(acecode::SessionStorage::get_project_dir(cwd)) {}
    ~ProjectCleanup() { std::filesystem::remove_all(project_dir); }
    std::string project_dir;
};

} // namespace

TEST(SessionTitle, GeneratedTitlePersistsWithGeneratedSource) {
    const auto cwd = temp_cwd("generated");
    ProjectCleanup cleanup(cwd.string());

    acecode::SessionManager sm;
    sm.start_session(cwd.string(), "test-provider", "test-model", "20260610-010203-abcd");

    ASSERT_TRUE(sm.mark_auto_title_generation_started());
    EXPECT_FALSE(sm.mark_auto_title_generation_started());
    ASSERT_TRUE(sm.try_set_generated_session_title("Fix auth timeout"));
    EXPECT_EQ(sm.current_title(), "Fix auth timeout");
    EXPECT_EQ(sm.current_title_source(), "generated");

    ASSERT_EQ(sm.ensure_active_session_id(), "20260610-010203-abcd");
    const auto meta = acecode::SessionStorage::read_meta(
        acecode::SessionStorage::meta_path(cleanup.project_dir, "20260610-010203-abcd"));
    EXPECT_EQ(meta.title, "Fix auth timeout");
    EXPECT_EQ(meta.title_source, "generated");
}

TEST(SessionTitle, UserTitleBlocksGeneratedOverwrite) {
    const auto cwd = temp_cwd("user_wins");
    ProjectCleanup cleanup(cwd.string());

    acecode::SessionManager sm;
    sm.start_session(cwd.string(), "test-provider", "test-model", "20260610-020304-abcd");

    sm.set_session_title("Manual title");
    EXPECT_FALSE(sm.try_set_generated_session_title("Generated title"));
    EXPECT_EQ(sm.current_title(), "Manual title");
    EXPECT_EQ(sm.current_title_source(), "user");

    sm.set_session_title("");
    EXPECT_EQ(sm.current_title(), "");
    EXPECT_EQ(sm.current_title_source(), "");
    EXPECT_FALSE(sm.try_set_generated_session_title("Generated after clear"));
}

TEST(SessionTitle, SanitizesJsonTitleOutput) {
    EXPECT_EQ(
        acecode::sanitize_generated_session_title(R"({"title":"  Refactor provider retry flow  "})"),
        "Refactor provider retry flow");
    EXPECT_EQ(acecode::sanitize_generated_session_title("Title:   Build web session title API"),
              "Build web session title API");
    EXPECT_TRUE(acecode::sanitize_generated_session_title(
        std::string("bad") + static_cast<char>(1) + "line").empty());
}
