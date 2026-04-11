#include "file_read_tool.hpp"
#include "mtime_tracker.hpp"
#include "utils/logger.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <filesystem>

namespace acecode {

static constexpr size_t MAX_FILE_SIZE = 10 * 1024 * 1024; // 10MB

static ToolResult execute_file_read(const std::string& arguments_json) {
    std::string file_path;
    int start_line = 0;
    int end_line = 0;

    try {
        auto args = nlohmann::json::parse(arguments_json);
        file_path = args.value("file_path", "");
        start_line = args.value("start_line", 0);
        end_line = args.value("end_line", 0);
    } catch (...) {
        return ToolResult{"[Error] Failed to parse tool arguments.", false};
    }

    if (file_path.empty()) {
        return ToolResult{"[Error] No file_path provided.", false};
    }

    LOG_DEBUG("file_read: path=" + file_path + " start=" + std::to_string(start_line) + " end=" + std::to_string(end_line));

    if (!std::filesystem::exists(file_path)) {
        return ToolResult{"[Error] File not found: " + file_path +
            "\nCurrent directory: " + std::filesystem::current_path().string(), false};
    }

    // Check file size
    auto file_size = std::filesystem::file_size(file_path);
    if (file_size > MAX_FILE_SIZE) {
        return ToolResult{"[Error] File too large (" + std::to_string(file_size / (1024*1024)) +
            "MB). Use start_line/end_line to read a portion, or use grep to search.", false};
    }

    // Read file
    std::ifstream ifs(file_path, std::ios::binary);
    if (!ifs.is_open()) {
        return ToolResult{"[Error] Cannot open file: " + file_path, false};
    }

    // Track mtime for later conflict detection
    MtimeTracker::instance().record_read(file_path);

    if (start_line > 0 || end_line > 0) {
        // Line range mode
        std::string line;
        std::ostringstream result;
        int line_num = 0;
        int start = start_line > 0 ? start_line : 1;
        int end = end_line > 0 ? end_line : std::numeric_limits<int>::max();

        while (std::getline(ifs, line)) {
            line_num++;
            if (line_num >= start && line_num <= end) {
                result << line_num << ": " << line << "\n";
            }
            if (line_num > end) break;
        }

        if (result.str().empty()) {
            return ToolResult{"[Error] No lines in range " + std::to_string(start) +
                "-" + std::to_string(end) + " (file has " + std::to_string(line_num) + " lines).", false};
        }

        return ToolResult{result.str(), true};
    }

    // Full file read
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return ToolResult{oss.str(), true};
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

    return ToolImpl{def, execute_file_read};
}

} // namespace acecode
