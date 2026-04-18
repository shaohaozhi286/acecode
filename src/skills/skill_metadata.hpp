#pragma once

#include <string>
#include <vector>
#include <filesystem>

namespace acecode {

struct SkillMetadata {
    std::string name;              // Normalized skill name (from frontmatter, or dir name)
    std::string command_key;       // Kebab-case command key (e.g. "my-plan" -> "/my-plan")
    std::string description;       // Short description shown in skills_list / /help
    std::string category;          // First-level directory under skills root, or "" if flat
    std::filesystem::path skill_md_path; // Absolute path to SKILL.md
    std::filesystem::path skill_dir;     // Absolute path to the skill directory
    std::vector<std::string> platforms;  // Empty = all platforms
    std::vector<std::string> tags;
};

} // namespace acecode
