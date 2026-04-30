#include "skills/default_skill_seeder.hpp"
#include "skills/skill_registry.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path make_temp_root(const std::string& name) {
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path root = fs::temp_directory_path() /
        ("acecode-default-skill-seeder-" + name + "-" + std::to_string(now));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    return root;
}

void write_skill_file(const fs::path& dir,
                      const std::string& name,
                      const std::string& description) {
    fs::create_directories(dir);
    std::ofstream ofs(dir / "SKILL.md", std::ios::binary);
    ofs << "---\n"
        << "name: " << name << "\n"
        << "description: " << description << "\n"
        << "---\n\n"
        << "# " << name << "\n\n"
        << description << "\n";
}

void write_seed_bundle(const fs::path& seed_root) {
    for (const auto& seed : acecode::default_skill_seeds()) {
        write_skill_file(seed_root / seed.relative_path,
                         seed.name,
                         "seeded " + seed.name);
    }
}

std::string read_file(const fs::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(ifs),
                       std::istreambuf_iterator<char>());
}

size_t count_outcome(const acecode::DefaultSkillSeedInstallResult& result,
                     const std::string& value) {
    size_t count = 0;
    for (const auto& outcome : result.outcomes) {
        if (outcome.result == value) ++count;
    }
    return count;
}

class DefaultSkillSeederTest : public ::testing::Test {
protected:
    fs::path root;
    fs::path home;
    fs::path seed_root;

    void SetUp() override {
        root = make_temp_root("case");
        home = root / "home" / ".acecode";
        seed_root = root / "seed" / "skills";
        write_seed_bundle(seed_root);
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

TEST_F(DefaultSkillSeederTest, InstallsAllDefaultSeedsIntoGlobalSkillRoot) {
    auto result = acecode::install_default_global_skills(home, seed_root, true);

    ASSERT_TRUE(result.attempted);
    ASSERT_TRUE(result.state_written);
    ASSERT_EQ(result.outcomes.size(), acecode::default_skill_seeds().size());
    EXPECT_EQ(count_outcome(result, "installed"), acecode::default_skill_seeds().size());

    for (const auto& seed : acecode::default_skill_seeds()) {
        EXPECT_TRUE(fs::is_regular_file(home / "skills" / seed.relative_path / "SKILL.md"));
    }

    auto state = nlohmann::json::parse(read_file(acecode::default_skill_seed_state_path(home)));
    EXPECT_EQ(state["bundle_version"], "2026-04-30.1");
    ASSERT_TRUE(state["skills"].is_array());
    EXPECT_EQ(state["skills"].size(), acecode::default_skill_seeds().size());
}

TEST_F(DefaultSkillSeederTest, SeededSkillsAreVisibleInSameRegistryScan) {
    auto result = acecode::install_default_global_skills(home, seed_root, true);
    ASSERT_TRUE(result.state_written);

    acecode::SkillRegistry registry;
    registry.set_scan_roots({home / "skills"});
    registry.scan();

    EXPECT_EQ(registry.list().size(), acecode::default_skill_seeds().size());
    for (const auto& seed : acecode::default_skill_seeds()) {
        auto found = registry.find(seed.name);
        ASSERT_TRUE(found.has_value()) << seed.name;
        EXPECT_EQ(found->description, "seeded " + seed.name);
    }
}

TEST_F(DefaultSkillSeederTest, SkipsExistingTargetsAndDoesNotOverwriteUserFiles) {
    const auto& first = acecode::default_skill_seeds().front();
    fs::path existing = home / "skills" / first.relative_path;
    write_skill_file(existing, first.name, "user copy");

    auto result = acecode::install_default_global_skills(home, seed_root, true);
    ASSERT_TRUE(result.state_written);
    EXPECT_EQ(count_outcome(result, "skipped"), 1u);
    EXPECT_EQ(count_outcome(result, "installed"), acecode::default_skill_seeds().size() - 1);
    EXPECT_NE(read_file(existing / "SKILL.md").find("user copy"), std::string::npos);

    auto second = acecode::install_default_global_skills(home, seed_root, true);
    ASSERT_TRUE(second.state_written);
    EXPECT_EQ(count_outcome(second, "skipped"), acecode::default_skill_seeds().size());
    EXPECT_EQ(count_outcome(second, "installed"), 0u);
    EXPECT_NE(read_file(existing / "SKILL.md").find("user copy"), std::string::npos);
}

TEST_F(DefaultSkillSeederTest, AgentRootDiscoveryAndPrecedenceSurviveSeeding) {
    auto result = acecode::install_default_global_skills(home, seed_root, true);
    ASSERT_TRUE(result.state_written);

    fs::path project_agent_root = root / "project" / ".agent" / "skills";
    write_skill_file(project_agent_root / "skill-management" / "find-skills",
                     "find-skills",
                     "project agent override");
    write_skill_file(project_agent_root / "ops" / "agent-only",
                     "agent-only",
                     "agent compatible skill");

    acecode::SkillRegistry registry;
    registry.set_scan_roots({project_agent_root, home / "skills"});
    registry.scan();

    auto overridden = registry.find("find-skills");
    ASSERT_TRUE(overridden.has_value());
    EXPECT_EQ(overridden->description, "project agent override");

    auto agent_only = registry.find("agent-only");
    ASSERT_TRUE(agent_only.has_value());
    EXPECT_EQ(agent_only->description, "agent compatible skill");
}

} // namespace
