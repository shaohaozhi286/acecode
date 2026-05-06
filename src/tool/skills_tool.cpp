#include "skills_tool.hpp"

#include "../skills/skill_registry.hpp"
#include "../utils/logger.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <set>

namespace acecode {

namespace {

std::string trim_ascii(std::string value) {
    auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(),
                std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
                value.end());
    return value;
}

std::vector<SkillMetadata> filter_skills_by_category(
    const std::vector<SkillMetadata>& skills, const std::string& category) {
    if (category.empty()) return skills;
    std::vector<SkillMetadata> filtered;
    for (const auto& skill : skills) {
        if (skill.category == category) filtered.push_back(skill);
    }
    return filtered;
}

nlohmann::json category_array_from_skills(
    const std::vector<SkillMetadata>& skills) {
    std::set<std::string> categories;
    for (const auto& skill : skills) {
        if (!skill.category.empty()) categories.insert(skill.category);
    }

    nlohmann::json out = nlohmann::json::array();
    for (const auto& category : categories) out.push_back(category);
    return out;
}

std::string join_json_string_array(const nlohmann::json& arr) {
    std::string joined;
    for (const auto& item : arr) {
        if (!item.is_string()) continue;
        if (!joined.empty()) joined += ", ";
        joined += item.get<std::string>();
    }
    return joined;
}

bool json_array_contains(const nlohmann::json& arr, const std::string& value) {
    for (const auto& item : arr) {
        if (item.is_string() && item.get<std::string>() == value) return true;
    }
    return false;
}

} // namespace

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

    auto execute = [&registry](const std::string& arguments_json, const ToolContext& /*ctx*/) -> ToolResult {
        std::string requested_category;
        try {
            if (!arguments_json.empty()) {
                auto args = nlohmann::json::parse(arguments_json);
                requested_category = args.value("category", "");
            }
        } catch (...) {
            return ToolResult{"[Error] Failed to parse tool arguments.", false};
        }

        const std::string normalized_category = trim_ascii(requested_category);
        const auto all_skills = registry.list();
        const auto available_categories = category_array_from_skills(all_skills);

        const bool has_category_filter = !normalized_category.empty();
        const bool known_category = json_array_contains(available_categories,
                                normalized_category);
        const bool fallback_applied = has_category_filter && !known_category;

        auto skills = fallback_applied
            ? all_skills
            : filter_skills_by_category(all_skills, normalized_category);

        std::string reason = "ok";
        if (all_skills.empty()) {
            reason = "registry_empty";
        } else if (fallback_applied) {
            reason = "fallback_from_invalid_filter";
        } else if (has_category_filter && skills.empty()) {
            reason = "empty_after_valid_filter";
        }

        nlohmann::json out;
        out["success"] = true;
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& s : skills) {
            nlohmann::json item;
            item["name"] = s.name;
            item["description"] = s.description;
            if (!s.category.empty()) {
                item["category"] = s.category;
            }
            arr.push_back(item);
        }
        out["skills"] = arr;
        out["count"] = skills.size();
        out["categories"] = category_array_from_skills(skills);
        out["available_categories"] = available_categories;
        out["reason"] = reason;
        out["fallback_applied"] = fallback_applied;
        if (!requested_category.empty()) out["requested_category"] = requested_category;
        if (!normalized_category.empty()) out["normalized_category"] = normalized_category;
        if (!fallback_applied && has_category_filter) out["applied_category"] = normalized_category;
        if (fallback_applied) out["invalid_category"] = normalized_category;

        if (reason == "registry_empty") {
            out["message"] = "No skills installed. Add SKILL.md files under ~/.acecode/skills/ or ~/.agent/skills/.";
        } else if (reason == "empty_after_valid_filter") {
            out["message"] = "No skills in category '" + normalized_category + "'.";
            if (!available_categories.empty()) {
                out["hint"] = "Available categories: " + join_json_string_array(available_categories) + ".";
            }
        } else if (reason == "fallback_from_invalid_filter") {
            out["message"] = "Ignoring invalid category filter '" + normalized_category + "'. Returning all installed skills.";
            out["hint"] = "Use one of the available categories or omit category to list all skills.";
        } else if (skills.empty()) {
            out["message"] = normalized_category.empty()
                ? "No skills installed. Add SKILL.md files under ~/.acecode/skills/ or ~/.agent/skills/."
                : ("No skills in category '" + normalized_category + "'.");
        } else {
            out["hint"] = "Use skill_view(name=...) to load the full content of a skill.";
        }

        LOG_DEBUG("[skills_list] requested_category='" + requested_category +
                  "' normalized_category='" + normalized_category +
                  "' fallback_applied=" + (fallback_applied ? std::string("true") : std::string("false")) +
                  " reason=" + reason +
                  " result_count=" + std::to_string(skills.size()) +
                  " available_categories=" + std::to_string(available_categories.size()));
        return ToolResult{out.dump(), true};
    };

    return ToolImpl{def, execute, /*is_read_only=*/true};
}

} // namespace acecode
