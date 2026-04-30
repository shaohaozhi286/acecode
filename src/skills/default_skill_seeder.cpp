#include "default_skill_seeder.hpp"

#include "../utils/logger.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <utility>

namespace fs = std::filesystem;

namespace acecode {

namespace {

constexpr const char* kSeedBundleVersion = "2026-04-30.1";
constexpr const char* kSeedStateFile = ".seed_skills_state.json";

bool seed_dir_has_all_skills(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return false;
    for (const auto& seed : default_skill_seeds()) {
        if (!fs::is_regular_file(dir / seed.relative_path / "SKILL.md", ec)) {
            return false;
        }
    }
    return true;
}

fs::path normalized_path(const fs::path& path) {
    std::error_code ec;
    fs::path normalized = fs::weakly_canonical(path, ec);
    if (ec) normalized = path.lexically_normal();
    return normalized;
}

std::optional<fs::path> valid_seed_dir(const fs::path& path) {
    fs::path normalized = normalized_path(path);
    if (seed_dir_has_all_skills(normalized)) return normalized;
    return std::nullopt;
}

std::optional<std::string> hash_file_fnv1a64(const fs::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return std::nullopt;

    uint64_t hash = 14695981039346656037ULL;
    char buf[4096];
    while (ifs.good()) {
        ifs.read(buf, sizeof(buf));
        std::streamsize n = ifs.gcount();
        for (std::streamsize i = 0; i < n; ++i) {
            hash ^= static_cast<unsigned char>(buf[i]);
            hash *= 1099511628211ULL;
        }
    }

    std::ostringstream oss;
    oss << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16) << hash;
    return oss.str();
}

bool copy_tree_no_overwrite(const fs::path& source_dir,
                            const fs::path& target_dir,
                            std::string& error) {
    std::error_code ec;
    if (fs::exists(target_dir, ec)) {
        error = "target_exists";
        return false;
    }
    fs::create_directories(target_dir, ec);
    if (ec) {
        error = "create_target_failed: " + ec.message();
        return false;
    }

    for (auto it = fs::recursive_directory_iterator(
             source_dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (ec) break;

        std::error_code rel_ec;
        fs::path rel = fs::relative(it->path(), source_dir, rel_ec);
        if (rel_ec) {
            error = "relative_path_failed: " + rel_ec.message();
            return false;
        }
        fs::path target = target_dir / rel;

        if (it->is_directory()) {
            fs::create_directories(target, ec);
            if (ec) {
                error = "create_directory_failed: " + ec.message();
                return false;
            }
            continue;
        }

        if (!it->is_regular_file()) continue;

        if (target.has_parent_path()) {
            fs::create_directories(target.parent_path(), ec);
            if (ec) {
                error = "create_parent_failed: " + ec.message();
                return false;
            }
        }
        if (fs::exists(target, ec)) {
            error = "target_file_exists";
            return false;
        }
        fs::copy_file(it->path(), target, fs::copy_options::none, ec);
        if (ec) {
            error = "copy_file_failed: " + ec.message();
            return false;
        }
    }

    if (ec) {
        error = "scan_source_failed: " + ec.message();
        return false;
    }
    return true;
}

void write_seed_state(DefaultSkillSeedInstallResult& result) {
    std::error_code ec;
    fs::create_directories(result.state_path.parent_path(), ec);
    if (ec) {
        result.error = "failed to create seed state directory: " + ec.message();
        return;
    }

    nlohmann::json j;
    j["bundle_version"] = kSeedBundleVersion;
    j["first_initialization"] = result.first_initialization;
    j["seed_skills_dir"] = result.seed_skills_dir.generic_string();
    j["target_root"] = result.target_root.generic_string();
    j["generated_at_unix"] = static_cast<long long>(std::time(nullptr));
    j["skills"] = nlohmann::json::array();

    for (const auto& outcome : result.outcomes) {
        nlohmann::json item;
        item["name"] = outcome.name;
        item["source_id"] = outcome.source_id;
        item["relative_path"] = outcome.relative_path;
        item["result"] = outcome.result;
        if (!outcome.message.empty()) item["message"] = outcome.message;
        if (!outcome.skill_md_hash.empty()) item["skill_md_hash"] = outcome.skill_md_hash;
        j["skills"].push_back(std::move(item));
    }

    std::ofstream ofs(result.state_path, std::ios::binary);
    if (!ofs.is_open()) {
        result.error = "failed to open seed state file";
        return;
    }
    ofs << j.dump(2) << '\n';
    result.state_written = true;
}

} // namespace

const std::vector<DefaultSkillSeed>& default_skill_seeds() {
    static const std::vector<DefaultSkillSeed> seeds = {
        {"find-skills",
         "claude-code-haha:find-skills@76d21ddf33ef7927294cdc019b83b6d263a19ac6",
         fs::path("skill-management") / "find-skills"},
        {"skill-installer",
         "codex-system:skill-installer@2026-04-30",
         fs::path("skill-management") / "skill-installer"},
        {"skill-creator",
         "codex-system:skill-creator@2026-04-30",
         fs::path("skill-management") / "skill-creator"},
        {"native-mcp",
         "hermes-agent:mcp/native-mcp@4eecaf06e48834e105cbd989ae0bae5a2a618c1d",
         fs::path("mcp") / "native-mcp"},
        {"mcporter",
         "hermes-agent:mcp/mcporter@4eecaf06e48834e105cbd989ae0bae5a2a618c1d",
         fs::path("mcp") / "mcporter"},
    };
    return seeds;
}

std::optional<fs::path> find_default_skill_seed_dir(const std::string& argv0_dir) {
    if (const char* env = std::getenv("ACECODE_SEED_SKILLS_DIR")) {
        if (env && *env) {
            if (auto found = valid_seed_dir(fs::path(env))) return found;
        }
    }

    if (!argv0_dir.empty()) {
        fs::path install_candidate = fs::path(argv0_dir) / ".." / "share" /
                                     "acecode" / "seed" / "skills";
        if (auto found = valid_seed_dir(install_candidate)) return found;

        fs::path probe = fs::path(argv0_dir);
        for (int i = 0; i < 5; ++i) {
            fs::path dev_candidate = probe / "assets" / "seed" / "skills";
            if (auto found = valid_seed_dir(dev_candidate)) return found;
            fs::path parent = probe.parent_path();
            if (parent == probe) break;
            probe = parent;
        }
    }

#ifndef _WIN32
    if (auto found = valid_seed_dir(fs::path("/usr/share/acecode/seed/skills"))) {
        return found;
    }
#endif

    return std::nullopt;
}

fs::path default_skill_seed_state_path(const fs::path& acecode_home) {
    return acecode_home / kSeedStateFile;
}

DefaultSkillSeedInstallResult install_default_global_skills(
    const fs::path& acecode_home,
    const fs::path& seed_skills_dir,
    bool first_initialization) {
    DefaultSkillSeedInstallResult result;
    result.first_initialization = first_initialization;
    result.seed_skills_dir = seed_skills_dir;
    result.target_root = acecode_home / "skills";
    result.state_path = default_skill_seed_state_path(acecode_home);

    if (!first_initialization) {
        return result;
    }

    result.attempted = true;

    std::error_code ec;
    fs::create_directories(result.target_root, ec);
    if (ec) {
        result.error = "failed to create global skills root: " + ec.message();
        return result;
    }

    for (const auto& seed : default_skill_seeds()) {
        DefaultSkillSeedOutcome outcome;
        outcome.name = seed.name;
        outcome.source_id = seed.source_id;
        outcome.relative_path = seed.relative_path.generic_string();

        fs::path source_dir = seed_skills_dir / seed.relative_path;
        fs::path source_md = source_dir / "SKILL.md";
        fs::path target_dir = result.target_root / seed.relative_path;

        if (!fs::is_regular_file(source_md, ec)) {
            outcome.result = "missing_source";
            outcome.message = source_md.generic_string();
            result.outcomes.push_back(std::move(outcome));
            continue;
        }

        if (fs::exists(target_dir, ec)) {
            outcome.result = "skipped";
            outcome.message = "target_exists";
            if (auto hash = hash_file_fnv1a64(target_dir / "SKILL.md")) {
                outcome.skill_md_hash = *hash;
            }
            result.outcomes.push_back(std::move(outcome));
            continue;
        }

        std::string copy_error;
        if (!copy_tree_no_overwrite(source_dir, target_dir, copy_error)) {
            outcome.result = copy_error == "target_exists" ? "skipped" : "error";
            outcome.message = copy_error;
            result.outcomes.push_back(std::move(outcome));
            continue;
        }

        outcome.result = "installed";
        if (auto hash = hash_file_fnv1a64(target_dir / "SKILL.md")) {
            outcome.skill_md_hash = *hash;
        }
        result.outcomes.push_back(std::move(outcome));
    }

    write_seed_state(result);
    return result;
}

DefaultSkillSeedInstallResult install_default_global_skills_on_first_initialization(
    const fs::path& acecode_home,
    const std::string& argv0_dir,
    bool first_initialization) {
    DefaultSkillSeedInstallResult result;
    result.first_initialization = first_initialization;
    result.target_root = acecode_home / "skills";
    result.state_path = default_skill_seed_state_path(acecode_home);

    if (!first_initialization) {
        return result;
    }

    auto seed_dir = find_default_skill_seed_dir(argv0_dir);
    if (!seed_dir) {
        result.attempted = true;
        result.error = "default skill seed bundle not found";
        LOG_WARN("[skills] Default skill seed bundle not found; skipping first-run skill seeding");
        return result;
    }

    return install_default_global_skills(acecode_home, *seed_dir, true);
}

} // namespace acecode
