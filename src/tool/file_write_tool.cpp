#include "file_write_tool.hpp"
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

static ToolResult execute_file_write(const std::string& arguments_json, const ToolContext& ctx) {
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

    if (ctx.track_file_write_before) {
        try {
            ctx.track_file_write_before(file_path);
        } catch (const std::exception& e) {
            LOG_WARN(std::string("file_write checkpoint hook failed: ") + e.what());
        } catch (...) {
            LOG_WARN("file_write checkpoint hook failed with unknown error");
        }
    }

    // Write content
    if (!FileOperations::write_content(file_path, content, error)) {
        return ToolResult{error, false};
    }

    // Update mtime tracker
    MtimeTracker::instance().record_write(file_path);

    if (!file_exists) {
        // Summary for a fresh file: count lines of new content as additions.
        int new_lines = 0;
        for (char c : content) if (c == '\n') ++new_lines;
        if (!content.empty() && content.back() != '\n') ++new_lines;

        ToolSummary summary;
        summary.verb = "Created";
        summary.object = file_path;
        summary.metrics.emplace_back("+", std::to_string(new_lines));
        summary.icon = tool_icon("file_write");

        // 新文件:走 "空 → 新内容" 路径得到全 Added 的 hunk,给 TUI 用。
        auto structured = generate_structured_diff("", content, file_path);

        ToolResult r{"Created file: " + file_path, true};
        r.summary = std::move(summary);
        r.hunks = std::move(structured);
        return r;
    }

    // 覆写场景:结构化 + 文本 diff 同源产出。
    DiffStats stats;
    std::string diff = generate_unified_diff(old_content, content, file_path, stats);
    auto structured = generate_structured_diff(old_content, content, file_path);

    ToolSummary summary;
    summary.verb = "Wrote";
    summary.object = file_path;
    summary.metrics.emplace_back("+", std::to_string(stats.additions));
    summary.metrics.emplace_back("-", std::to_string(stats.deletions));
    summary.icon = tool_icon("file_write");

    ToolResult r{"Updated file: " + file_path + "\n\n" + diff, true};
    r.summary = std::move(summary);
    r.hunks = std::move(structured);
    return r;
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
