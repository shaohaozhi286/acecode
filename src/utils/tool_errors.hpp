#pragma once

#include <string>

namespace acecode {

// Standardized error messages for tools
class ToolErrors {
public:
    static std::string parse_failed() {
        return "[Error] Failed to parse tool arguments.";
    }

    static std::string missing_parameter(const std::string& param_name) {
        return "[Error] Required parameter missing: " + param_name;
    }

    static std::string file_not_found(const std::string& path, const std::string& cwd = "") {
        std::string msg = "[Error] File not found: " + path;
        if (!cwd.empty()) {
            msg += "\nCurrent directory: " + cwd;
        }
        return msg;
    }

    static std::string cannot_open_file(const std::string& path) {
        return "[Error] Cannot open file: " + path;
    }

    static std::string cannot_write_file(const std::string& path) {
        return "[Error] Cannot write to file: " + path;
    }

    static std::string file_too_large(size_t size_mb, const std::string& suggestion = "") {
        std::string msg = "[Error] File too large (" + std::to_string(size_mb) + "MB).";
        if (!suggestion.empty()) {
            msg += " " + suggestion;
        }
        return msg;
    }

    static std::string external_modification(const std::string& path) {
        return "[Error] File was modified externally since it was last read. "
               "Re-read the file before writing to avoid data loss: " + path;
    }

    static std::string string_not_found(const std::string& file_path) {
        return "[Error] old_string not found in " + file_path +
               ". Re-read the file and make sure to use the exact string, "
               "including whitespace and indentation.";
    }

    static std::string string_not_unique(size_t count, const std::string& file_path) {
        return "[Error] old_string found " + std::to_string(count) +
               " times in " + file_path + ". Include more surrounding lines "
               "to uniquely identify the target location.";
    }

    static std::string empty_parameter(const std::string& param_name) {
        return "[Error] " + param_name + " cannot be empty.";
    }

    static std::string no_lines_in_range(int start, int end, int total_lines) {
        return "[Error] No lines in range " + std::to_string(start) +
               "-" + std::to_string(end) + " (file has " + std::to_string(total_lines) + " lines).";
    }
};

} // namespace acecode
