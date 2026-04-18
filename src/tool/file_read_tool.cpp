#include "file_read_tool.hpp"
#include "mtime_tracker.hpp"
#include "utils/logger.hpp"
#include "utils/tool_args_parser.hpp"
#include "utils/tool_errors.hpp"
#include "utils/file_operations.hpp"
#include <nlohmann/json.hpp>

namespace acecode {

static ToolResult execute_file_read(const std::string& arguments_json) {
    // Parse arguments
    ToolArgsParser parser(arguments_json);
    if (parser.has_error()) {
        return ToolResult{parser.error(), false};
    }

    std::string file_path = parser.get_or<std::string>("file_path", "");
    int start_line = parser.get_or<int>("start_line", 0);
    int end_line = parser.get_or<int>("end_line", 0);

    if (file_path.empty()) {
        return ToolResult{ToolErrors::missing_parameter("file_path"), false};
    }

    LOG_DEBUG("file_read: path=" + file_path + " start=" + std::to_string(start_line) + " end=" + std::to_string(end_line));

    // Check file exists
    auto exists_check = FileOperations::check_file_exists(file_path);
    if (!exists_check.success) {
        return exists_check;
    }

    // Check file size
    auto size_check = FileOperations::check_file_size(file_path,
        "Use start_line/end_line to read a portion, or use grep to search.");
    if (!size_check.success) {
        return size_check;
    }

    // Track mtime for later conflict detection
    MtimeTracker::instance().record_read(file_path);

    std::string content;
    std::string error;

    if (start_line > 0 || end_line > 0) {
        // Line range mode
        if (!FileOperations::read_lines(file_path, start_line, end_line, content, error)) {
            return ToolResult{error, false};
        }
    } else {
        // Full file read
        if (!FileOperations::read_content(file_path, content, error)) {
            return ToolResult{error, false};
        }
    }

    return ToolResult{content, true};
}

ToolImpl create_file_read_tool() {
    ToolDef def;
    def.name = "file_read";
    def.description = "Read the contents of a file. Use start_line/end_line for partial reads. "
                      "Always use absolute paths.";
    def.parameters = nlohmann::json({
        {"type", "object"},
        {"properties", {
            {"file_path", {
                {"type", "string"},
                {"description", "Absolute path to the file to read"}
            }},
            {"start_line", {
                {"type", "integer"},
                {"description", "Start line number (1-indexed, inclusive). Optional."}
            }},
            {"end_line", {
                {"type", "integer"},
                {"description", "End line number (1-indexed, inclusive). Optional."}
            }}
        }},
        {"required", nlohmann::json::array({"file_path"})}
    });

    return ToolImpl{def, execute_file_read, /*is_read_only=*/true};
}

} // namespace acecode
