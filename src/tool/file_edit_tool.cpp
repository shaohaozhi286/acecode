#include "file_edit_tool.hpp"
#include "mtime_tracker.hpp"
#include "diff_utils.hpp"
#include "utils/logger.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <filesystem>

namespace acecode {

static ToolResult execute_file_edit(const std::string& arguments_json) {
    std::string file_path;
    std::string old_string;
    std::string new_string;

    try {
        auto args = nlohmann::json::parse(arguments_json);
        file_path = args.value("file_path", "");
        old_string = args.value("old_string", "");
        new_string = args.value("new_string", "");
    } catch (...) {
        return ToolResult{"[Error] Failed to parse tool arguments.", false};
    }

    if (file_path.empty()) {
        return ToolResult{"[Error] No file_path provided.", false};
    }
    if (old_string.empty()) {
        return ToolResult{"[Error] old_string cannot be empty.", false};
    }

    LOG_DEBUG("file_edit: path=" + file_path + " old_len=" + std::to_string(old_string.size()) + " new_len=" + std::to_string(new_string.size()));

    if (!std::filesystem::exists(file_path)) {
        return ToolResult{"[Error] File not found: " + file_path +
            ". Use file_write to create a new file.", false};
    }

    // Mtime conflict check
    if (MtimeTracker::instance().was_externally_modified(file_path)) {
        return ToolResult{"[Error] File was modified externally since it was last read. "
            "Re-read the file before editing: " + file_path, false};
    }

    // Read file
    std::ifstream ifs(file_path, std::ios::binary);
    if (!ifs.is_open()) {
        return ToolResult{"[Error] Cannot open file: " + file_path, false};
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    std::string content = oss.str();
    ifs.close();

    // Find all occurrences
    size_t count = 0;
    size_t pos = 0;
    size_t found_pos = std::string::npos;
    while ((pos = content.find(old_string, pos)) != std::string::npos) {
        if (count == 0) found_pos = pos;
        count++;
        pos += old_string.size();
    }

    if (count == 0) {
        return ToolResult{"[Error] old_string not found in " + file_path +
            ". Re-read the file and make sure to use the exact string, "
            "including whitespace and indentation.", false};
    }

    if (count > 1) {
        return ToolResult{"[Error] old_string found " + std::to_string(count) +
            " times in " + file_path + ". Include more surrounding lines "
            "to uniquely identify the target location.", false};
    }

    // Apply edit
    std::string old_content = content;
    content.replace(found_pos, old_string.size(), new_string);

    // Write back
    std::ofstream ofs(file_path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        return ToolResult{"[Error] Cannot write to file: " + file_path, false};
    }
    ofs << content;
    ofs.close();

    // Update mtime tracker
    MtimeTracker::instance().record_write(file_path);

    // Generate diff
    std::string diff = generate_unified_diff(old_content, content, file_path);
    return ToolResult{"Edited " + file_path + "\n\n" + diff, true};
}

ToolImpl create_file_edit_tool() {
    ToolDef def;
    def.name = "file_edit";
    def.description = "Edit a file by replacing an exact string with a new string. "
                      "The old_string must appear exactly once in the file. "
                      "Include surrounding context lines to ensure uniqueness. "
                      "Always use absolute paths.";
    def.parameters = nlohmann::json({
        {"type", "object"},
        {"properties", {
            {"file_path", {
                {"type", "string"},
                {"description", "Absolute path to the file to edit"}
            }},
            {"old_string", {
                {"type", "string"},
                {"description", "The exact string to find and replace (must match exactly once)"}
            }},
            {"new_string", {
                {"type", "string"},
                {"description", "The replacement string"}
            }}
        }},
        {"required", nlohmann::json::array({"file_path", "old_string", "new_string"})}
    });

    return ToolImpl{def, execute_file_edit};
}

} // namespace acecode
