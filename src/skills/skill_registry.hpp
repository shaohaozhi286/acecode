#pragma once

#include "skill_metadata.hpp"

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace acecode {

// Discovery + in-memory index for SKILL.md documents. Thread-safe for the
// common pattern of "scan once at startup from UI thread, read from worker
// threads via list()/find()".
class SkillRegistry {
public:
    SkillRegistry() = default;

    // Set the scan roots before calling scan(). First entry is typically
    // ~/.acecode/skills/, later entries come from config.skills.external_dirs.
    void set_scan_roots(std::vector<std::filesystem::path> roots);

    // Set the disabled-name set. Skills with matching name are silently omitted
    // from scan results.
    void set_disabled(std::unordered_set<std::string> disabled);

    // Recursively scan every root for SKILL.md, deduplicate by skill name
    // (first-seen wins, local roots first), drop platform-incompatible or
    // disabled skills, and populate the internal list.
    void scan();

    // Re-run scan(). Equivalent but named for /skills reload clarity.
    void reload() { scan(); }

    // Return an immutable copy of the current skill list, optionally filtered
    // by category.
    std::vector<SkillMetadata> list(const std::string& category = "") const;

    // Look up a skill by name or by command_key. Returns nullopt if missing.
    std::optional<SkillMetadata> find(const std::string& name_or_key) const;

    // Read the full SKILL.md body (post-frontmatter) for a skill.
    std::string read_skill_body(const std::string& name) const;

    // Return relative paths of supporting files under references/, templates/,
    // scripts/, assets/ (in that order). Empty when none exist.
    std::vector<std::string> list_supporting_files(const std::string& name) const;

    // Resolve an absolute path to a file inside the skill directory. Rejects
    // ".." traversal. Returns nullopt when the skill or file does not exist
    // or escapes the skill directory.
    std::optional<std::filesystem::path> resolve_skill_file(
        const std::string& name, const std::string& relative_path) const;

private:
    mutable std::mutex mu_;
    std::vector<std::filesystem::path> roots_;
    std::unordered_set<std::string> disabled_;
    std::vector<SkillMetadata> skills_;
};

} // namespace acecode
