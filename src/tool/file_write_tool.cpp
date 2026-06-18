#include "file_write_tool.hpp"
#include "mtime_tracker.hpp"
#include "diff_utils.hpp"
#include "tool_icons.hpp"
#include "utils/logger.hpp"
#include "utils/tool_args_parser.hpp"
#include "utils/tool_errors.hpp"
#include "utils/text_file_buffer.hpp"
#include "utils/utf8_path.hpp"
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

    auto write_guard = MtimeTracker::instance().acquire_write_guard(file_path);
    bool file_exists = std::filesystem::exists(path_from_utf8(file_path));

    // Read old content for diff (if overwriting)
    std::string old_content;
    TextFileMetadata write_metadata = default_new_file_text_metadata();
    if (file_exists) {
        auto read_result = read_text_file_buffer(file_path);
        if (!read_result.success) {
            auto metadata = MtimeTracker::instance().read_metadata(file_path);
            if (metadata && metadata->lossy) {
                return ToolResult{ToolErrors::file_read_not_safe_for_edit(file_path), false};
            }
            return ToolResult{read_result.error, false};
        }
        old_content = read_result.buffer.text;
        write_metadata = read_result.buffer.metadata;

        const auto read_check =
            MtimeTracker::instance().validate_read_baseline_for_edit(file_path, old_content);
        switch (read_check.status) {
            case MtimeTracker::ReadBaselineStatus::Ok:
                break;
            case MtimeTracker::ReadBaselineStatus::NotRead:
                return ToolResult{ToolErrors::file_not_read_for_edit(file_path), false};
            case MtimeTracker::ReadBaselineStatus::UnsafeRead:
                return ToolResult{ToolErrors::file_read_not_safe_for_edit(file_path), false};
            case MtimeTracker::ReadBaselineStatus::ExternallyModified:
                return ToolResult{ToolErrors::external_modification(file_path), false};
        }
    }

    auto before_write = [&](const std::string& path) {
        if (ctx.track_file_write_before) {
            try {
                ctx.track_file_write_before(path);
            } catch (const std::exception& e) {
                LOG_WARN(std::string("file_write checkpoint hook failed: ") + e.what());
            } catch (...) {
                LOG_WARN("file_write checkpoint hook failed with unknown error");
            }
        }
    };

    std::string normalized_content = normalize_text_to_lf(content);
    auto write_result = safe_write_text_file(file_path, normalized_content, write_metadata, before_write);
    if (!write_result.success) {
        return ToolResult{write_result.error, false};
    }

    // Update mtime tracker
    MtimeTracker::instance().record_write(file_path, normalized_content);

    if (!file_exists) {
        // Summary for a fresh file: count lines of new content as additions.
        int new_lines = 0;
        for (char c : normalized_content) if (c == '\n') ++new_lines;
        if (!normalized_content.empty() && normalized_content.back() != '\n') ++new_lines;

        ToolSummary summary;
        summary.verb = "Created";
        summary.object = file_path;
        summary.metrics.emplace_back("+", std::to_string(new_lines));
        summary.icon = tool_icon("file_write");

        // 新文件:走 "空 → 新内容" 路径得到全 Added 的 hunk,给 TUI 用。
        auto structured = generate_structured_diff("", normalized_content, file_path);

        ToolResult r{"Created file: " + file_path, true};
        r.summary = std::move(summary);
        r.hunks = std::move(structured);
        return r;
    }

    // 覆写场景:结构化 + 文本 diff 同源产出。
    DiffStats stats;
    std::string diff = generate_unified_diff(old_content, normalized_content, file_path, stats);
    auto structured = generate_structured_diff(old_content, normalized_content, file_path);

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
    def.description = "Writes a file to the local filesystem. "
                      "This tool will overwrite the existing file if there is one at the provided path. "
                      "If this is an existing file, you MUST use the file_read tool first to read the file's contents. This tool will fail if you did not read the file first. "
                      "Creates parent directories as needed. "
                      "When overwriting an existing text file, preserves its detected encoding and line endings; "
                      "new files default to UTF-8 without BOM. "
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
