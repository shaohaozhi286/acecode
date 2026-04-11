#include "file_write_tool.hpp"
#include "mtime_tracker.hpp"
#include "diff_utils.hpp"
#include "utils/logger.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <sstream>
#include <filesystem>

namespace acecode {

static ToolResult execute_file_write(const std::string& arguments_json) {
    std::string file_path;
    std::string content;

    try {
        auto args = nlohmann::json::parse(arguments_json);
        file_path = args.value("file_path", "");
        content = args.value("content", "");
    } catch (...) {
        return ToolResult{"[Error] Failed to parse tool arguments.", false};
    }

    if (file_path.empty()) {
        return ToolResult{"[Error] No file_path provided.", false};
    }

    LOG_DEBUG("file_write: path=" + file_path + " content_len=" + std::to_string(content.size()));

    bool file_exists = std::filesystem::exists(file_path);

    // Mtime conflict check for existing files
    if (file_exists) {
        if (MtimeTracker::instance().was_externally_modified(file_path)) {
            return ToolResult{"[Error] File was modified externally since it was last read. "
                "Re-read the file before writing to avoid data loss: " + file_path, false};
        }
    }

    // Read old content for diff (if overwriting)
    std::string old_content;
    if (file_exists) {
        std::ifstream ifs(file_path, std::ios::binary);
        if (ifs.is_open()) {
            std::ostringstream oss;
            oss << ifs.rdbuf();
            old_content = oss.str();
        }
    }

    // Create parent directories if needed
    auto parent = std::filesystem::path(file_path).parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent)) {
        std::filesystem::create_directories(parent);
    }

    // Write content
    std::ofstream ofs(file_path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
        return ToolResult{"[Error] Cannot open file for writing: " + file_path, false};
    }
    ofs << content;
    ofs.close();

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

    return ToolImpl{def, execute_file_write};
}

} // namespace acecode
