#pragma once

#include "tool_errors.hpp"
#include "tool/tool_executor.hpp"
#include "tool/mtime_tracker.hpp"
#include <string>
#include <fstream>
#include <sstream>
#include <filesystem>

namespace acecode {

// Common file operation utilities for tools
class FileOperations {
public:
    static constexpr size_t MAX_FILE_SIZE = 10 * 1024 * 1024; // 10MB

    // Check if file exists, return error ToolResult if not
    static ToolResult check_file_exists(const std::string& path) {
        if (!std::filesystem::exists(path)) {
            return ToolResult{
                ToolErrors::file_not_found(path, std::filesystem::current_path().string()),
                false
            };
        }
        return ToolResult{"", true};
    }

    // Check file size, return error ToolResult if too large
    static ToolResult check_file_size(const std::string& path, const std::string& suggestion = "") {
        auto file_size = std::filesystem::file_size(path);
        if (file_size > MAX_FILE_SIZE) {
            return ToolResult{
                ToolErrors::file_too_large(file_size / (1024 * 1024), suggestion),
                false
            };
        }
        return ToolResult{"", true};
    }

    // Check for external modifications using MtimeTracker
    static ToolResult check_mtime_conflict(const std::string& path) {
        if (MtimeTracker::instance().was_externally_modified(path)) {
            return ToolResult{ToolErrors::external_modification(path), false};
        }
        return ToolResult{"", true};
    }

    // Read entire file content
    static bool read_content(const std::string& path, std::string& out_content, std::string& out_error) {
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs.is_open()) {
            out_error = ToolErrors::cannot_open_file(path);
            return false;
        }
        std::ostringstream oss;
        oss << ifs.rdbuf();
        out_content = oss.str();
        return true;
    }

    // Write content to file (creates parent dirs if needed)
    static bool write_content(const std::string& path, const std::string& content, std::string& out_error) {
        // Create parent directories if needed
        auto parent = std::filesystem::path(path).parent_path();
        if (!parent.empty() && !std::filesystem::exists(parent)) {
            try {
                std::filesystem::create_directories(parent);
            } catch (const std::filesystem::filesystem_error& e) {
                out_error = "[Error] Cannot create parent directory: " + std::string(e.what());
                return false;
            }
        }

        std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) {
            out_error = ToolErrors::cannot_write_file(path);
            return false;
        }
        ofs << content;
        return true;
    }

    // Read file content with line range
    static bool read_lines(const std::string& path, int start_line, int end_line,
                           std::string& out_content, std::string& out_error) {
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs.is_open()) {
            out_error = ToolErrors::cannot_open_file(path);
            return false;
        }

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
            out_error = ToolErrors::no_lines_in_range(start, end, line_num);
            return false;
        }

        out_content = result.str();
        return true;
    }
};

} // namespace acecode
