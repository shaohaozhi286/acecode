#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace acecode {

struct DefaultSkillSeed {
    std::string name;
    std::string source_id;
    std::filesystem::path relative_path;
};

struct DefaultSkillSeedOutcome {
    std::string name;
    std::string source_id;
    std::string relative_path;
    std::string result;
    std::string message;
    std::string skill_md_hash;
};

struct DefaultSkillSeedInstallResult {
    bool attempted = false;
    bool first_initialization = false;
    bool state_written = false;
    std::string error;
    std::filesystem::path seed_skills_dir;
    std::filesystem::path target_root;
    std::filesystem::path state_path;
    std::vector<DefaultSkillSeedOutcome> outcomes;
};

const std::vector<DefaultSkillSeed>& default_skill_seeds();

std::optional<std::filesystem::path> find_default_skill_seed_dir(
    const std::string& argv0_dir = "");

std::filesystem::path default_skill_seed_state_path(
    const std::filesystem::path& acecode_home);

DefaultSkillSeedInstallResult install_default_global_skills(
    const std::filesystem::path& acecode_home,
    const std::filesystem::path& seed_skills_dir,
    bool first_initialization);

DefaultSkillSeedInstallResult install_default_global_skills_on_first_initialization(
    const std::filesystem::path& acecode_home,
    const std::string& argv0_dir,
    bool first_initialization);

} // namespace acecode
