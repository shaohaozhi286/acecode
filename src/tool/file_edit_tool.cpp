#include "file_edit_tool.hpp"
#include "mtime_tracker.hpp"
#include "diff_utils.hpp"
#include "tool_icons.hpp"
#include "utils/logger.hpp"
#include "utils/tool_args_parser.hpp"
#include "utils/tool_errors.hpp"
#include "utils/file_operations.hpp"
#include <nlohmann/json.hpp>
#include <exception>
#include <filesystem>

namespace acecode {

static ToolResult execute_file_edit(const std::string& arguments_json, const ToolContext& ctx) {
    // Parse arguments
    ToolArgsParser parser(arguments_json);
    if (parser.has_error()) {
        return ToolResult{parser.error(), false};
    }

    std::string file_path = parser.get_or<std::string>("file_path", "");
    std::string old_string = parser.get_or<std::string>("old_string", "");
    std::string new_string = parser.get_or<std::string>("new_string", "");

    if (file_path.empty()) {
        return ToolResult{ToolErrors::missing_parameter("file_path"), false};
    }
    if (old_string.empty()) {
        return ToolResult{ToolErrors::empty_parameter("old_string"), false};
    }

    LOG_DEBUG("file_edit: path=" + file_path + " old_len=" + std::to_string(old_string.size()) + " new_len=" + std::to_string(new_string.size()));

    // Check file exists
    auto exists_check = FileOperations::check_file_exists(file_path);
    if (!exists_check.success) {
        return ToolResult{exists_check.output + ". Use file_write to create a new file.", false};
    }

    // Mtime conflict check
    auto conflict_check = FileOperations::check_mtime_conflict(file_path);
    if (!conflict_check.success) {
        return conflict_check;
    }

    // Read file
    std::string content;
    std::string error;
    if (!FileOperations::read_content(file_path, content, error)) {
        return ToolResult{error, false};
    }

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
        return ToolResult{ToolErrors::string_not_found(file_path), false};
    }

    if (count > 1) {
        return ToolResult{ToolErrors::string_not_unique(count, file_path), false};
    }

    // Apply edit
    std::string old_content = content;
    content.replace(found_pos, old_string.size(), new_string);

    if (ctx.track_file_write_before) {
        try {
            ctx.track_file_write_before(file_path);
        } catch (const std::exception& e) {
            LOG_WARN(std::string("file_edit checkpoint hook failed: ") + e.what());
        } catch (...) {
            LOG_WARN("file_edit checkpoint hook failed with unknown error");
        }
    }

    // Write back
    if (!FileOperations::write_content(file_path, content, error)) {
        return ToolResult{error, false};
    }

    // Update mtime tracker
    MtimeTracker::instance().record_write(file_path);

    // 同时产出结构化 hunk + 文本 diff,保证 TUI 彩色渲染和 LLM 下一轮阅读同源。
    DiffStats stats;
    std::string diff = generate_unified_diff(old_content, content, file_path, stats);
    auto structured = generate_structured_diff(old_content, content, file_path);

    ToolSummary summary;
    summary.verb = "Edited";
    summary.object = file_path;
    summary.metrics.emplace_back("+", std::to_string(stats.additions));
    summary.metrics.emplace_back("-", std::to_string(stats.deletions));
    summary.icon = tool_icon("file_edit");

    ToolResult r{"Edited " + file_path + "\n\n" + diff, true};
    r.summary = std::move(summary);
    r.hunks = std::move(structured);
    return r;
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

    return ToolImpl{def, execute_file_edit, /*is_read_only=*/false};
}

} // namespace acecode
