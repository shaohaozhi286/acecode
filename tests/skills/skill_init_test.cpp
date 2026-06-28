#include "config/config.hpp"
#include "skills/skill_init.hpp"
#include "skills/skill_registry.hpp"
#include "utils/paths.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <stdlib.h>
#endif

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
constexpr const char* kHomeEnvName = "USERPROFILE";
#else
constexpr const char* kHomeEnvName = "HOME";
#endif

void set_env_value(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void clear_env_value(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

struct EnvSnapshot {
    std::string name;
    bool had_value = false;
    std::string value;
};

EnvSnapshot capture_env(const char* name) {
    EnvSnapshot snapshot;
    snapshot.name = name;
    if (const char* existing = std::getenv(name)) {
        snapshot.had_value = true;
        snapshot.value = existing;
    }
    return snapshot;
}

void restore_env(const EnvSnapshot& snapshot) {
    if (snapshot.had_value) {
        set_env_value(snapshot.name.c_str(), snapshot.value);
    } else {
        clear_env_value(snapshot.name.c_str());
    }
}

fs::path make_temp_root() {
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path root = fs::temp_directory_path() /
                    ("acecode-skill-init-test-" + std::to_string(now));
    std::error_code ec;
    fs::remove_all(root, ec);
    return root;
}

void write_skill(const fs::path& skills_root,
                 const std::string& name,
                 const std::string& description) {
    fs::path dir = skills_root / name;
    fs::create_directories(dir);
    std::ofstream ofs(dir / "SKILL.md", std::ios::binary);
    ofs << "---\n"
        << "name: " << name << "\n"
        << "description: " << description << "\n"
        << "---\n\n"
        << "# " << name << "\n";
}

class SkillInitOpencodeTest : public ::testing::Test {
protected:
    fs::path root;
    fs::path home;
    fs::path workspace;
    std::vector<EnvSnapshot> env_snapshots;

    void SetUp() override {
        acecode::reset_run_mode_for_test();
        env_snapshots = {
            capture_env(kHomeEnvName),
            capture_env("HOME"),
            capture_env("USERPROFILE"),
            capture_env("XDG_CONFIG_HOME"),
            capture_env("XDG_CACHE_HOME"),
            capture_env("OPENCODE_CONFIG_DIR"),
        };

        root = make_temp_root();
        home = root / "home";
        workspace = home / "work" / "repo";
        fs::create_directories(workspace);

        set_env_value(kHomeEnvName, home.string());
        set_env_value("HOME", home.string());
        set_env_value("USERPROFILE", home.string());
        set_env_value("XDG_CONFIG_HOME", (home / "xdg-config").string());
        set_env_value("XDG_CACHE_HOME", (home / "xdg-cache").string());
        set_env_value("OPENCODE_CONFIG_DIR", (home / "custom-opencode").string());
    }

    void TearDown() override {
        for (const auto& snapshot : env_snapshots) {
            restore_env(snapshot);
        }
        acecode::reset_run_mode_for_test();
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    void initialize(acecode::SkillRegistry& registry,
                    const acecode::AppConfig& cfg = {}) const {
        acecode::initialize_skill_registry(registry, cfg, workspace.string());
    }
};

TEST_F(SkillInitOpencodeTest, DiscoversOpencodeProjectGlobalCustomAndCacheSkillsByDefault) {
    write_skill(workspace / ".opencode" / "skills", "opencode-project", "project opencode");
    write_skill(home / "xdg-config" / "opencode" / "skills",
                "opencode-global", "global opencode");
    write_skill(home / "custom-opencode" / "skills",
                "opencode-custom", "custom opencode");
    write_skill(home / "xdg-cache" / "opencode" / "skills",
                "opencode-cache", "cache opencode");
    write_skill(workspace / ".agents" / "skills",
                "opencode-agents", "agents compat");
    write_skill(workspace / ".claude" / "skills",
                "opencode-claude", "claude compat");

    acecode::SkillRegistry registry;
    initialize(registry);

    auto expect_description = [&](const std::string& name, const std::string& description) {
        SCOPED_TRACE(name);
        auto found = registry.find(name);
        ASSERT_TRUE(found.has_value());
        EXPECT_EQ(found->description, description);
    };
    expect_description("opencode-project", "project opencode");
    expect_description("opencode-global", "global opencode");
    expect_description("opencode-custom", "custom opencode");
    expect_description("opencode-cache", "cache opencode");
    expect_description("opencode-agents", "agents compat");
    expect_description("opencode-claude", "claude compat");
}

TEST_F(SkillInitOpencodeTest, DisabledReuseOpencodeSkipsOnlyOpencodeRoots) {
    write_skill(workspace / ".acecode" / "skills", "native-skill", "native skill");
    write_skill(workspace / ".opencode" / "skills", "opencode-project", "project opencode");

    acecode::AppConfig cfg;
    cfg.skills.reuse_opencode = false;
    acecode::SkillRegistry registry;
    initialize(registry, cfg);

    ASSERT_TRUE(registry.find("native-skill").has_value());
    EXPECT_EQ(registry.find("native-skill")->description, "native skill");
    EXPECT_FALSE(registry.find("opencode-project").has_value());
}

TEST_F(SkillInitOpencodeTest, NativeProjectSkillWinsOverOpencodeProjectSkill) {
    write_skill(workspace / ".acecode" / "skills", "shared-skill", "from native acecode");
    write_skill(workspace / ".opencode" / "skills", "shared-skill", "from opencode");

    acecode::SkillRegistry registry;
    initialize(registry);

    auto found = registry.find("shared-skill");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->description, "from native acecode");
}

} // namespace
