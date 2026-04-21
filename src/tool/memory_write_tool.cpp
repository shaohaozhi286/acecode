#include "memory_write_tool.hpp"

#include "../memory/memory_paths.hpp"
#include "../memory/memory_registry.hpp"
#include "../memory/memory_types.hpp"
#include "../utils/logger.hpp"

#include <nlohmann/json.hpp>

namespace acecode {

namespace {

MemoryWriteMode parse_mode(const std::string& s) {
    if (s == "create") return MemoryWriteMode::Create;
    if (s == "update") return MemoryWriteMode::Update;
    return MemoryWriteMode::Upsert;
}

} // namespace

ToolImpl create_memory_write_tool(MemoryRegistry& registry) {
    ToolDef def;
    def.name = "memory_write";
    def.description =
        "Persist a memory entry under ~/.acecode/memory/<name>.md. Writes are "
        "atomic and the MEMORY.md index is updated automatically. 'name' is "
        "sanitized to [A-Za-z0-9_-] (1-64 chars). 'type' must be one of "
        "user|feedback|project|reference. 'mode' defaults to 'upsert' (also "
        "'create' or 'update'). The tool always writes inside "
        "~/.acecode/memory/ — other paths are rejected.";
    def.parameters = nlohmann::json({
        {"type", "object"},
        {"properties", {
            {"name", {
                {"type", "string"},
                {"description", "File-stem identifier ([A-Za-z0-9_-]{1,64})."}
            }},
            {"type", {
                {"type", "string"},
                {"enum", nlohmann::json::array({"user", "feedback", "project", "reference"})},
                {"description", "Category of the memory."}
            }},
            {"description", {
                {"type", "string"},
                {"description", "Short one-line description used in MEMORY.md index."}
            }},
            {"body", {
                {"type", "string"},
                {"description", "Markdown body of the memory entry."}
            }},
            {"mode", {
                {"type", "string"},
                {"enum", nlohmann::json::array({"create", "update", "upsert"})},
                {"description", "Write semantics. Default 'upsert' (create-or-replace)."}
            }}
        }},
        {"required", nlohmann::json::array({"name", "type", "description", "body"})}
    });

    auto execute = [&registry](const std::string& arguments_json,
                               const ToolContext& /*ctx*/) -> ToolResult {
        std::string name, type_str, description, body, mode_str;
        try {
            if (arguments_json.empty()) {
                return ToolResult{"[Error] memory_write requires arguments.", false};
            }
            auto args = nlohmann::json::parse(arguments_json);
            name = args.value("name", "");
            type_str = args.value("type", "");
            description = args.value("description", "");
            body = args.value("body", "");
            mode_str = args.value("mode", "upsert");
        } catch (...) {
            return ToolResult{"[Error] Failed to parse tool arguments.", false};
        }

        std::string name_err = validate_memory_name(name);
        if (!name_err.empty()) {
            nlohmann::json err;
            err["success"] = false;
            err["error"] = name_err;
            return ToolResult{err.dump(), false};
        }
        auto parsed_type = parse_memory_type(type_str);
        if (!parsed_type.has_value()) {
            nlohmann::json err;
            err["success"] = false;
            err["error"] = "invalid type: " + type_str +
                           " (allowed: user|feedback|project|reference)";
            return ToolResult{err.dump(), false};
        }
        if (description.empty()) {
            nlohmann::json err;
            err["success"] = false;
            err["error"] = "description must not be empty";
            return ToolResult{err.dump(), false};
        }

        std::string err_msg;
        auto written = registry.upsert(name, *parsed_type, description, body,
                                       parse_mode(mode_str), err_msg);
        if (!written) {
            nlohmann::json err;
            err["success"] = false;
            err["error"] = err_msg;
            return ToolResult{err.dump(), false};
        }

        nlohmann::json out;
        out["success"] = true;
        out["name"] = written->name;
        out["description"] = written->description;
        out["type"] = memory_type_to_string(written->type);
        out["path"] = written->path.generic_string();
        LOG_INFO("[memory_write] persisted " + written->path.generic_string());
        return ToolResult{out.dump(), true};
    };

    return ToolImpl{def, execute, /*is_read_only=*/false};
}

} // namespace acecode
