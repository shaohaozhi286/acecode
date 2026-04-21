#include "memory_read_tool.hpp"

#include "../memory/memory_registry.hpp"
#include "../memory/memory_types.hpp"
#include "../utils/logger.hpp"

#include <nlohmann/json.hpp>

namespace acecode {

ToolImpl create_memory_read_tool(MemoryRegistry& registry,
                                 std::size_t max_index_bytes) {
    ToolDef def;
    def.name = "memory_read";
    def.description =
        "Read persistent user memory from ~/.acecode/memory/. "
        "With no arguments, returns the MEMORY.md index plus a list of all entries "
        "(name, description, type). With only 'type', filters entries by one of "
        "user|feedback|project|reference. With 'name', returns that entry's full body. "
        "Missing entries return {found:false} rather than erroring. Read-only.";
    def.parameters = nlohmann::json({
        {"type", "object"},
        {"properties", {
            {"name", {
                {"type", "string"},
                {"description", "Memory entry file stem (the <name> in <name>.md)."}
            }},
            {"type", {
                {"type", "string"},
                {"enum", nlohmann::json::array({"user", "feedback", "project", "reference"})},
                {"description", "Filter entries by type when 'name' is absent."}
            }}
        }},
        {"required", nlohmann::json::array()}
    });

    auto execute = [&registry, max_index_bytes](const std::string& arguments_json,
                                                const ToolContext& /*ctx*/) -> ToolResult {
        std::string name, type_str;
        try {
            if (!arguments_json.empty()) {
                auto args = nlohmann::json::parse(arguments_json);
                name = args.value("name", "");
                type_str = args.value("type", "");
            }
        } catch (...) {
            return ToolResult{"[Error] Failed to parse tool arguments.", false};
        }

        if (!name.empty()) {
            auto found = registry.find(name);
            nlohmann::json out;
            if (!found) {
                out["found"] = false;
                out["name"] = name;
                out["hint"] = "Use memory_read() with no args to list available entries.";
                LOG_DEBUG("[memory_read] entry not found: " + name);
                return ToolResult{out.dump(), true};
            }
            out["found"] = true;
            out["name"] = found->name;
            out["description"] = found->description;
            out["type"] = memory_type_to_string(found->type);
            out["body"] = found->body;
            out["path"] = found->path.generic_string();
            return ToolResult{out.dump(), true};
        }

        std::optional<MemoryType> type_filter;
        if (!type_str.empty()) {
            auto parsed = parse_memory_type(type_str);
            if (!parsed) {
                nlohmann::json err;
                err["success"] = false;
                err["error"] = "invalid type: " + type_str +
                               " (allowed: user|feedback|project|reference)";
                return ToolResult{err.dump(), false};
            }
            type_filter = *parsed;
        }

        auto entries = registry.list(type_filter);
        nlohmann::json out;
        out["success"] = true;
        if (!type_filter.has_value()) {
            out["index"] = registry.read_index_raw(max_index_bytes);
        }
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& e : entries) {
            nlohmann::json item;
            item["name"] = e.name;
            item["description"] = e.description;
            item["type"] = memory_type_to_string(e.type);
            arr.push_back(item);
        }
        out["entries"] = arr;
        out["count"] = entries.size();
        if (entries.empty()) {
            out["message"] = type_filter.has_value()
                ? ("No memory entries of type '" + type_str + "'.")
                : "No memory entries yet. Use memory_write to create one.";
        }
        return ToolResult{out.dump(), true};
    };

    return ToolImpl{def, execute, /*is_read_only=*/true};
}

} // namespace acecode
