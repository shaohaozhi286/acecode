#include "skills_tool.hpp"

#include "../skills/skill_registry.hpp"
#include "../utils/logger.hpp"

#include <nlohmann/json.hpp>

#include <set>

namespace acecode {

ToolImpl create_skills_list_tool(SkillRegistry& registry) {
    ToolDef def;
    def.name = "skills_list";
    def.description = "List available skills (name, description, category). "
                      "Returns only minimal metadata — use skill_view to load full SKILL.md content. "
                      "Optional 'category' argument filters by category directory.";
    def.parameters = nlohmann::json({
        {"type", "object"},
        {"properties", {
            {"category", {
                {"type", "string"},
                {"description", "Optional category name (top-level directory under skills root)."}
            }}
        }},
        {"required", nlohmann::json::array()}
    });

    auto execute = [&registry](const std::string& arguments_json) -> ToolResult {
        std::string category;
        try {
            if (!arguments_json.empty()) {
                auto args = nlohmann::json::parse(arguments_json);
                category = args.value("category", "");
            }
        } catch (...) {
            return ToolResult{"[Error] Failed to parse tool arguments.", false};
        }

        auto skills = registry.list(category);

        nlohmann::json out;
        out["success"] = true;
        nlohmann::json arr = nlohmann::json::array();
        std::set<std::string> cats;
        for (const auto& s : skills) {
            nlohmann::json item;
            item["name"] = s.name;
            item["description"] = s.description;
            if (!s.category.empty()) {
                item["category"] = s.category;
                cats.insert(s.category);
            }
            arr.push_back(item);
        }
        out["skills"] = arr;
        out["count"] = skills.size();
        nlohmann::json cats_arr = nlohmann::json::array();
        for (const auto& c : cats) cats_arr.push_back(c);
        out["categories"] = cats_arr;
        if (skills.empty()) {
            out["message"] = category.empty()
                ? "No skills installed. Add SKILL.md files under ~/.acecode/skills/."
                : ("No skills in category '" + category + "'.");
        } else {
            out["hint"] = "Use skill_view(name=...) to load the full content of a skill.";
        }

        LOG_DEBUG("[skills_list] returned " + std::to_string(skills.size()) + " skill(s)");
        return ToolResult{out.dump(), true};
    };

    return ToolImpl{def, execute, /*is_read_only=*/true};
}

} // namespace acecode
