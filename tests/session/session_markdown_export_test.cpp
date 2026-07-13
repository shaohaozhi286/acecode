#include "session/session_markdown_export.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace acecode {

TEST(SessionMarkdownExportTest, BuildsMetadataRolesToolCallsAndStructuredContent) {
    SessionMeta meta;
    meta.id = "session-1";
    meta.title = "导出测试";
    meta.cwd = "C:/repo";
    meta.created_at = "2026-07-12T10:00:00Z";
    meta.updated_at = "2026-07-12T10:01:00Z";
    meta.provider = "openai";
    meta.model = "gpt-test";

    ChatMessage user;
    user.role = "user";
    user.content = "expanded prompt";
    user.timestamp = "2026-07-12T10:00:01Z";
    user.metadata = nlohmann::json{{"display_text", "原始问题"}, {"source", "test"}};

    ChatMessage assistant;
    assistant.role = "assistant";
    assistant.content = "我来检查。";
    assistant.tool_calls = nlohmann::json::array({nlohmann::json{
        {"id", "call-1"},
        {"function", {{"name", "file_read"}, {"arguments", R"({"path":"README.md"})"}}},
    }});

    ChatMessage tool;
    tool.role = "tool";
    tool.content = "文件内容";
    tool.tool_call_id = "call-1";
    tool.content_parts = nlohmann::json::array({nlohmann::json{{"type", "text"}, {"text", "文件内容"}}});

    const auto markdown = session_export::build_markdown(meta, {user, assistant, tool});
    EXPECT_NE(markdown.find("# 导出测试"), std::string::npos);
    EXPECT_NE(markdown.find("原始问题"), std::string::npos);
    EXPECT_EQ(markdown.find("expanded prompt"), std::string::npos);
    EXPECT_NE(markdown.find("工具调用参数"), std::string::npos);
    EXPECT_NE(markdown.find("file_read"), std::string::npos);
    EXPECT_NE(markdown.find("README.md"), std::string::npos);
    EXPECT_NE(markdown.find("工具调用 ID: `call-1`"), std::string::npos);
    EXPECT_NE(markdown.find("结构化内容"), std::string::npos);
    EXPECT_NE(markdown.find("2026-07-12T10:00:01Z"), std::string::npos);
}

TEST(SessionMarkdownExportTest, SanitizesWindowsFilenameAndAvoidsCollision) {
    EXPECT_EQ(session_export::sanitize_filename_stem("CON: invalid? ", "fallback"), "CON_ invalid_");
    EXPECT_EQ(session_export::sanitize_filename_stem("...", "session-1"), "session-1");

    const auto dir = std::filesystem::temp_directory_path() / "acecode-session-export-test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    ASSERT_FALSE(ec);
    {
        std::ofstream(dir / "My Session.md") << "existing";
    }
    EXPECT_EQ(session_export::choose_markdown_filename(dir, "My Session", "sid"), "My Session (2).md");
    std::filesystem::remove_all(dir, ec);
}

TEST(SessionMarkdownExportTest, KeepsVisibleSystemAndErrorRoles) {
    SessionMeta meta;
    meta.id = "sid";
    ChatMessage system;
    system.role = "system";
    system.content = "系统信息";
    ChatMessage error;
    error.role = "error";
    error.content = "失败";

    const auto markdown = session_export::build_markdown(meta, {system, error});
    EXPECT_NE(markdown.find("## 系统"), std::string::npos);
    EXPECT_NE(markdown.find("## 错误"), std::string::npos);
    EXPECT_NE(markdown.find("系统信息"), std::string::npos);
    EXPECT_NE(markdown.find("失败"), std::string::npos);
}

} // namespace acecode
