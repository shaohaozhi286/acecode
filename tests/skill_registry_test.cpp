#include "skills/skill_registry.hpp"
#include "utils/encoding.hpp"
#include "utils/utf8_path.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

void write_skill(const fs::path& root,
                 const std::string& category,
                 const std::string& name,
                 const std::string& description) {
    fs::path dir = root / acecode::path_from_utf8(category) / acecode::path_from_utf8(name);
    fs::create_directories(dir);
    std::ofstream ofs(dir / "SKILL.md", std::ios::binary);
    ofs << "---\n"
        << "name: " << name << "\n"
        << "description: " << description << "\n"
        << "---\n\n"
        << "# " << name << "\n\n"
        << description << "\n";
}

class SkillRegistryCompatTest : public ::testing::Test {
protected:
    fs::path temp_root;

    void SetUp() override {
        temp_root = fs::temp_directory_path() / fs::path("acecode-skill-registry-test");
        std::error_code ec;
        fs::remove_all(temp_root, ec);
        fs::create_directories(temp_root);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(temp_root, ec);
    }
};

TEST_F(SkillRegistryCompatTest, LoadsSkillFromAgentRoot) {
    fs::path acecode_root = temp_root / "acecode-root";
    fs::path agent_root = temp_root / "agent-root";
    write_skill(agent_root, "engineering", "agent-only", "loaded from .agent root");

    acecode::SkillRegistry registry;
    registry.set_scan_roots({acecode_root, agent_root});
    registry.scan();

    auto found = registry.find("agent-only");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->name, "agent-only");
    EXPECT_EQ(found->description, "loaded from .agent root");
}

TEST_F(SkillRegistryCompatTest, PrefersEarlierRootForDuplicateSkillNames) {
    fs::path acecode_root = temp_root / "acecode-root";
    fs::path agent_root = temp_root / "agent-root";
    write_skill(acecode_root, "engineering", "shared-skill", "from acecode root");
    write_skill(agent_root, "engineering", "shared-skill", "from agent root");

    acecode::SkillRegistry registry;
    registry.set_scan_roots({acecode_root, agent_root});
    registry.scan();

    auto found = registry.find("shared-skill");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->description, "from acecode root");
    EXPECT_EQ(registry.list().size(), 1u);
}

TEST_F(SkillRegistryCompatTest, PreservesUtf8CategoryDescriptionAndBody) {
    fs::path root = temp_root / "skills-root";
    write_skill(root, u8"中文分类", "unicode-skill", u8"处理中文描述");

    acecode::SkillRegistry registry;
    registry.set_scan_roots({root});
    registry.scan();

    auto found = registry.find("unicode-skill");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->category, u8"中文分类");
    EXPECT_EQ(found->description, u8"处理中文描述");
    EXPECT_TRUE(acecode::is_valid_utf8(found->category));
    EXPECT_TRUE(acecode::is_valid_utf8(found->description));

    std::string body = registry.read_skill_body("unicode-skill");
    EXPECT_NE(body.find(u8"处理中文描述"), std::string::npos);
    EXPECT_TRUE(acecode::is_valid_utf8(body));
}

} // namespace
