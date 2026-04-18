#include "skill_view_tool.hpp"

#include "../skills/skill_registry.hpp"
#include "../utils/logger.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace acecode {

namespace {

constexpr size_t MAX_FILE_SIZE = 2 * 1024 * 1024; // 2MB cap for SKILL files

nlohmann::json available_skills_list(const SkillRegistry& registry) {
    auto all = registry.list();
    nlohmann::json arr = nlohmann::json::array();
    const size_t limit = 20;
    for (size_t i = 0; i < all.size() && i < limit; ++i) {
        arr.push_back(all[i].name);
    }
    return arr;
}

} // namespace

ToolImpl create_skill_view_tool(SkillRegistry& registry) {
    ToolDef def;
    def.name = "skill_view";
    def.description = "Load the full content of a skill. Without file_path, returns the SKILL.md body. "
                      "With file_path, returns the content of a supporting file inside the skill's "
                      "references/, templates/, scripts/, or assets/ directory. "
                      "Path traversal ('..') is rejected.";
    def.parameters = nlohmann::json({
        {"type", "object"},
        {"properties", {
            {"name", {
                {"type", "string"},
                {"description", "Skill name (as returned by skills_list)."}
            }},
            {"file_path", {
                {"type", "string"},
                {"description", "Optional path relative to the skill directory (e.g. 'references/api.md')."}
            }}
        }},
        {"required", nlohmann::json::array({"name"})}
    });

    auto execute = [&registry](const std::string& arguments_json, const ToolContext& /*ctx*/) -> ToolResult {
        std::string name;
        std::string file_path;
        try {
            auto args = nlohmann::json::parse(arguments_json);
            name = args.value("name", "");
            file_path = args.value("file_path", "");
        } catch (...) {
            return ToolResult{"[Error] Failed to parse tool arguments.", false};
        }

        if (name.empty()) {
            nlohmann::json err;
            err["success"] = false;
            err["error"] = "Missing required argument 'name'.";
            return ToolResult{err.dump(), false};
        }

        auto meta = registry.find(name);
        if (!meta) {
            nlohmann::json err;
            err["success"] = false;
            err["error"] = "Skill '" + name + "' not found.";
            err["available_skills"] = available_skills_list(registry);
            err["hint"] = "Call skills_list to see all installed skills.";
            return ToolResult{err.dump(), false};
        }

        if (!file_path.empty()) {
            if (file_path.find("..") != std::string::npos) {
                nlohmann::json err;
                err["success"] = false;
                err["error"] = "Path traversal ('..') is not allowed.";
                return ToolResult{err.dump(), false};
            }
            auto resolved = registry.resolve_skill_file(name, file_path);
            if (!resolved) {
                nlohmann::json err;
                err["success"] = false;
                err["error"] = "File '" + file_path + "' not found or outside skill directory.";
                err["supporting_files"] = registry.list_supporting_files(name);
                return ToolResult{err.dump(), false};
            }
            std::error_code ec;
            if (!fs::exists(*resolved, ec) || !fs::is_regular_file(*resolved, ec)) {
                nlohmann::json err;
                err["success"] = false;
                err["error"] = "File '" + file_path + "' does not exist.";
                err["supporting_files"] = registry.list_supporting_files(name);
                return ToolResult{err.dump(), false};
            }
            auto size = fs::file_size(*resolved, ec);
            if (!ec && size > MAX_FILE_SIZE) {
                nlohmann::json err;
                err["success"] = false;
                err["error"] = "File too large to return inline.";
                return ToolResult{err.dump(), false};
            }
            std::ifstream ifs(*resolved, std::ios::binary);
            std::ostringstream oss;
            oss << ifs.rdbuf();
            nlohmann::json out;
            out["success"] = true;
            out["name"] = meta->name;
            out["file_path"] = file_path;
            out["content"] = oss.str();
            LOG_DEBUG("[skill_view] loaded supporting file " + file_path + " for skill " + meta->name);
            return ToolResult{out.dump(), true};
        }

        // Main SKILL.md body path.
        std::string body = registry.read_skill_body(meta->name);
        if (body.empty()) {
            std::ifstream ifs(meta->skill_md_path, std::ios::binary);
            std::ostringstream oss;
            oss << ifs.rdbuf();
            body = oss.str();
        }
        auto supporting = registry.list_supporting_files(meta->name);

        nlohmann::json out;
        out["success"] = true;
        out["name"] = meta->name;
        out["description"] = meta->description;
        if (!meta->category.empty()) out["category"] = meta->category;
        out["content"] = body;
        nlohmann::json files_arr = nlohmann::json::array();
        for (const auto& f : supporting) files_arr.push_back(f);
        out["linked_files"] = files_arr;
        LOG_DEBUG("[skill_view] loaded SKILL.md for " + meta->name + " (" +
                  std::to_string(body.size()) + " bytes, " +
                  std::to_string(supporting.size()) + " supporting files)");
        return ToolResult{out.dump(), true};
    };

    return ToolImpl{def, execute, /*is_read_only=*/true};
}

} // namespace acecode
