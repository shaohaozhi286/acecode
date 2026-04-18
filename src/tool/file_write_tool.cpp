#include "file_write_tool.hpp"
#include "mtime_tracker.hpp"
#include "diff_utils.hpp"
#include "utils/logger.hpp"
#include "utils/tool_args_parser.hpp"
#include "utils/tool_errors.hpp"
#include "utils/file_operations.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>

namespace acecode {

static ToolResult execute_file_write(const std::string& arguments_json) {
    // Parse arguments
    ToolArgsParser parser(arguments_json);
    if (parser.has_error()) {
        return ToolResult{parser.error(), false};
    }

    std::string file_path = parser.get_or<std::string>("file_path", "");
    std::string content = parser.get_or<std::string>("content", "");

    if (file_path.empty()) {
        return ToolResult{ToolErrors::missing_parameter("file_path"), false};
    }

    LOG_DEBUG("file_write: path=" + file_path + " content_len=" + std::to_string(content.size()));

    bool file_exists = std::filesystem::exists(file_path);

    // Mtime conflict check for existing files
    if (file_exists) {
        auto conflict_check = FileOperations::check_mtime_conflict(file_path);
        if (!conflict_check.success) {
            return conflict_check;
        }
    }

    // Read old content for diff (if overwriting)
    std::string old_content;
    std::string error;
    if (file_exists) {
        FileOperations::read_content(file_path, old_content, error);
        // Ignore read errors here, we'll still try to write
    }

    // Write content
    if (!FileOperations::write_content(file_path, content, error)) {
        return ToolResult{error, false};
    }

    // Update mtime tracker
    MtimeTracker::instance().record_write(file_path);

    if (!file_exists) {
        return ToolResult{"Created file: " + file_path, true};
    }

    // Generate diff for overwrite
    std::string diff = generate_unified_diff(old_content, content, file_path);
    return ToolResult{"Updated file: " + file_path + "\n\n" + diff, true};
}

ToolImpl create_file_write_tool() {
    ToolDef def;
    def.name = "file_write";
    def.description = "Write content to a file. Creates the file if it doesn't exist, "
                      "or overwrites it if it does. Creates parent directories as needed. "
                      "Always use absolute paths.";
    def.parameters = nlohmann::json({
        {"type", "object"},
        {"properties", {
            {"file_path", {
                {"type", "string"},
                {"description", "Absolute path to the file to write"}
            }},
            {"content", {
                {"type", "string"},
                {"description", "The full content to write to the file"}
            }}
        }},
        {"required", nlohmann::json::array({"file_path", "content"})}
    });

    return ToolImpl{def, execute_file_write, /*is_read_only=*/false};
}

} // namespace acecode
