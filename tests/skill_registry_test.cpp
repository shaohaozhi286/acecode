#include "skills/skill_registry.hpp"
#include "utils/encoding.hpp"
#include "utils/utf8_path.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

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

void append_utf16le(std::string& out, char16_t ch) {
    out.push_back(static_cast<char>(ch & 0xFF));
    out.push_back(static_cast<char>((ch >> 8) & 0xFF));
}

std::string utf16le_bom_bytes(std::u16string_view text) {
    std::string out;
    out.push_back(static_cast<char>(0xFF));
    out.push_back(static_cast<char>(0xFE));
    for (char16_t ch : text) append_utf16le(out, ch);
    return out;
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

TEST_F(SkillRegistryCompatTest, LoadsUtf16LegacyHeaderSkill) {
    fs::path root = temp_root / "skills-root";
    fs::path dir = root / "general" / "calculator";
    fs::create_directories(dir);
    std::ofstream ofs(dir / "SKILL.md", std::ios::binary);
    ofs << utf16le_bom_bytes(
        u"name: calculator\r\n"
        u"description: A simple calculator skill\r\n"
        u"category: general\r\n"
        u"\r\n"
        u"---\r\n"
        u"\r\n"
        u"# Calculator Skill\r\n");
    ofs.close();

    acecode::SkillRegistry registry;
    registry.set_scan_roots({root});
    registry.scan();

    auto found = registry.find("calculator");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->name, "calculator");
    EXPECT_EQ(found->description, "A simple calculator skill");
    EXPECT_EQ(found->category, "general");
    EXPECT_TRUE(acecode::is_valid_utf8(found->description));

    std::string body = registry.read_skill_body("calculator");
    EXPECT_NE(body.find("# Calculator Skill"), std::string::npos);
    EXPECT_TRUE(acecode::is_valid_utf8(body));
}

} // namespace
