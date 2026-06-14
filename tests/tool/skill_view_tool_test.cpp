// 覆盖 skill_view 的 TUI 精简输出:成功结果必须带 ToolSummary,但完整
// SKILL.md / supporting file 内容仍保留在 ToolResult.output 给模型使用。

#include <gtest/gtest.h>

#include "config/config.hpp"
#include "skills/skill_registry.hpp"
#include "tool/skill_view_tool.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <random>

namespace fs = std::filesystem;

namespace {

fs::path make_temp_root(const std::string& hint) {
    auto root = fs::temp_directory_path() /
        ("acecode_skill_view_tool_" + hint + "_" + std::to_string(std::random_device{}()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    return root;
}

void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream ofs(path, std::ios::binary);
    ofs << content;
}

void write_skill(const fs::path& root) {
    const fs::path dir = root / "engineering" / "openspec-explore";
    write_file(dir / "SKILL.md",
        "---\n"
        "name: openspec-explore\n"
        "description: Explore mode\n"
        "---\n\n"
        "# Explore\n\n"
        "Detailed instructions that must remain available to the model.\n");
    write_file(dir / "references" / "notes.md", "supporting reference body\n");
}

class SkillViewToolTest : public ::testing::Test {
protected:
    void SetUp() override {
        root = make_temp_root("root");
        write_skill(root);
        registry.set_scan_roots({root});
        registry.scan();
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    fs::path root;
    acecode::SkillRegistry registry;
};

} // namespace

TEST_F(SkillViewToolTest, SkillBodyKeepsFullOutputButAddsCompactSummary) {
    auto tool = acecode::create_skill_view_tool(registry);
    auto result = tool.execute(R"({"name":"openspec-explore"})", acecode::ToolContext{});

    ASSERT_TRUE(result.success);
    ASSERT_TRUE(result.summary.has_value());
    EXPECT_EQ(result.summary->verb, "skill_view");
    EXPECT_EQ(result.summary->object, "openspec-explore loaded");

    auto payload = nlohmann::json::parse(result.output);
    EXPECT_TRUE(payload.value("success", false));
    EXPECT_EQ(payload.value("name", ""), "openspec-explore");
    EXPECT_NE(payload.value("content", "").find("Detailed instructions"),
              std::string::npos);
    ASSERT_TRUE(payload.contains("linked_files"));
    EXPECT_EQ(payload["linked_files"].size(), 1u);
}

TEST_F(SkillViewToolTest, SupportingFileKeepsFullOutputButAddsCompactSummary) {
    auto tool = acecode::create_skill_view_tool(registry);
    auto result = tool.execute(
        R"({"name":"openspec-explore","file_path":"references/notes.md"})",
        acecode::ToolContext{});

    ASSERT_TRUE(result.success);
    ASSERT_TRUE(result.summary.has_value());
    EXPECT_EQ(result.summary->verb, "skill_view");
    EXPECT_EQ(result.summary->object,
              "openspec-explore references/notes.md loaded");

    auto payload = nlohmann::json::parse(result.output);
    EXPECT_TRUE(payload.value("success", false));
    EXPECT_EQ(payload.value("file_path", ""), "references/notes.md");
    EXPECT_EQ(payload.value("content", ""), "supporting reference body\n");
}

TEST_F(SkillViewToolTest, ToolContextCwdLoadsWorkspaceLocalSkill) {
    fs::path daemon_root = make_temp_root("daemon-root");
    fs::path workspace = make_temp_root("workspace-root");
    fs::path skill_dir = workspace / ".agent" / "skills" /
                         "engineering" / "workspace-only-skill-view";
    write_file(skill_dir / "SKILL.md",
        "---\n"
        "name: workspace-only-skill-view\n"
        "description: Workspace only\n"
        "---\n\n"
        "# Workspace Skill\n\n"
        "Workspace-specific instructions.\n");
    write_file(skill_dir / "references" / "extra.md", "workspace reference\n");

    acecode::SkillRegistry fallback_registry;
    fallback_registry.set_scan_roots({daemon_root / ".agent" / "skills"});
    fallback_registry.scan();

    acecode::AppConfig cfg;
    auto tool = acecode::create_skill_view_tool(fallback_registry, &cfg);
    acecode::ToolContext ctx;
    ctx.cwd = workspace.string();
    auto result = tool.execute(R"({"name":"workspace-only-skill-view"})", ctx);

    ASSERT_TRUE(result.success);
    auto payload = nlohmann::json::parse(result.output);
    EXPECT_EQ(payload.value("name", ""), "workspace-only-skill-view");
    EXPECT_NE(payload.value("content", "").find("Workspace-specific instructions"),
              std::string::npos);
    ASSERT_TRUE(payload.contains("linked_files"));
    ASSERT_EQ(payload["linked_files"].size(), 1u);
    EXPECT_EQ(payload["linked_files"][0].get<std::string>(), "references/extra.md");

    std::error_code ec;
    fs::remove_all(daemon_root, ec);
    fs::remove_all(workspace, ec);
}
