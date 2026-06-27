#include "file_read_tool.hpp"
#include "mtime_tracker.hpp"
#include "tool_icons.hpp"
#include "utils/logger.hpp"
#include "utils/tool_args_parser.hpp"
#include "utils/tool_errors.hpp"
#include "utils/file_operations.hpp"
#include "utils/text_file_buffer.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <limits>
#include <sstream>

namespace acecode {

static constexpr size_t FILE_READ_LARGE_HINT_THRESHOLD = 200 * 1024; // 200 KB
static constexpr const char* FILE_UNCHANGED_STUB =
    "File unchanged since last read.";

namespace {

static std::string format_read_metadata_footer(const FileReadEditMetadata& metadata) {
    std::ostringstream oss;
    oss << "\n<acecode-read-metadata"
        << " encoding=\"" << metadata.encoding << "\""
        << " line_endings=\"" << metadata.line_ending << "\"";
    if (metadata.start_line > 0 && metadata.end_line > 0) {
        oss << " range=\"" << metadata.start_line << "-" << metadata.end_line << "\"";
    }
    if (metadata.lossy) {
        oss << " lossy=\"true\""
            << " replacements=\"" << metadata.lossy_replacement_count << "\""
            << " editable=\"false\"";
    }
    oss << " />\n";
    return oss.str();
}

static std::string format_file_unchanged_stub(
    const MtimeTracker::ReadObservation& observation
) {
    std::ostringstream oss;
    oss << FILE_UNCHANGED_STUB
        << " The previous file_read result for this same file/range is still current.";
    if (!observation.tool_call_id.empty()) {
        oss << "\nPrevious file_read tool_call_id: " << observation.tool_call_id;
    }
    if (!observation.persisted_output_path.empty()) {
        oss << "\nFull previous output path: " << observation.persisted_output_path
            << "\nIf full content is needed, call file_read on that saved output path.";
    }
    oss << "\nDo not call file_read on the original file/range again unless the file changed or a different range is needed.";
    return oss.str();
}

static std::string format_numbered_range(const std::string& lf_text,
                                         int start_line,
                                         int end_line,
                                         int& out_start,
                                         int& out_end,
                                         int& out_total,
                                         bool& ok) {
    std::vector<std::string> lines = split_lf_lines_preserve_empty(lf_text);
    out_total = static_cast<int>(lines.size());
    int start = start_line > 0 ? start_line : 1;
    int end = end_line > 0 ? end_line : std::numeric_limits<int>::max();
    end = std::min(end, out_total);
    ok = out_total > 0 && start <= end && start <= out_total;
    if (!ok) return {};

    std::ostringstream oss;
    for (int line = start; line <= end; ++line) {
        oss << line << ": " << lines[static_cast<size_t>(line - 1)] << "\n";
    }
    out_start = start;
    out_end = end;
    return oss.str();
}

} // namespace

static ToolResult execute_file_read(const std::string& arguments_json, const ToolContext& /*ctx*/) {
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

    auto unchanged_observation =
        MtimeTracker::instance().unchanged_read_observation(file_path, start_line, end_line);
    if (unchanged_observation.has_value()) {
        ToolSummary summary;
        summary.verb = "Read";
        summary.object = file_path;
        summary.metrics.emplace_back("cache", "unchanged");
        summary.icon = tool_icon("file_read");

        ToolResult r{format_file_unchanged_stub(*unchanged_observation), true};
        r.summary = std::move(summary);
        return r;
    }

    auto read_result = read_text_file_buffer(file_path, true);
    if (!read_result.success) {
        return ToolResult{read_result.error, false};
    }

    const TextFileBuffer& buffer = read_result.buffer;
    std::string content;
    const bool partial_read = (start_line > 0 || end_line > 0);
    int displayed_line_count = 0;
    FileReadEditMetadata metadata;
    metadata.encoding = text_encoding_label(buffer.metadata.encoding);
    if (buffer.metadata.lossy) {
        metadata.encoding += " (lossy)";
        metadata.lossy = true;
        metadata.lossy_replacement_count = buffer.metadata.lossy_replacement_count;
    }
    metadata.line_ending = line_ending_label(buffer.metadata.line_ending);

    if (partial_read) {
        int actual_start = 0;
        int actual_end = 0;
        int total_lines = 0;
        bool ok = false;
        content = format_numbered_range(buffer.text, start_line, end_line,
                                        actual_start, actual_end, total_lines, ok);
        if (!ok) {
            int start = start_line > 0 ? start_line : 1;
            int end = end_line > 0 ? end_line : total_lines;
            return ToolResult{ToolErrors::no_lines_in_range(start, end, total_lines), false};
        }
        displayed_line_count = actual_end - actual_start + 1;
        if (!buffer.metadata.lossy) {
            metadata.start_line = actual_start;
            metadata.end_line = actual_end;
        }
    } else {
        content = buffer.text;
        for (char c : content) if (c == '\n') ++displayed_line_count;
        if (!content.empty() && content.back() != '\n') ++displayed_line_count;
        if (!buffer.metadata.lossy) {
            metadata.start_line = content.empty() ? 0 : 1;
            metadata.end_line = displayed_line_count;
        }
    }

    // 写入校验基于模型真实看到的 UTF-8/LF 文本,避免拿原始字节和规范化文本比较。
    MtimeTracker::instance().record_read(
        file_path, buffer.text, partial_read || buffer.metadata.lossy, metadata);
    MtimeTracker::instance().record_read_observation(file_path, start_line, end_line);

    // Large-file hint: only when the caller asked for the whole file (both
    // range bounds omitted) and the payload exceeds 200 KB. Appended as a
    // trailing hint line so the LLM sees the suggestion in-band, and tagged
    // on the summary so the TUI can mark the row.
    bool hint_added = false;
    if (!partial_read && buffer.text.size() > FILE_READ_LARGE_HINT_THRESHOLD) {
        const size_t kb = buffer.text.size() / 1024;
        if (!content.empty() && content.back() != '\n') content += "\n";
        content += "[hint: file is large (" + std::to_string(kb) +
                   "KB). Consider using start_line / end_line to narrow the read next time.]";
        hint_added = true;
    }

    if (buffer.metadata.lossy) {
        if (!content.empty() && content.back() != '\n') content += "\n";
        content += "[note: decoded with " +
                   std::to_string(buffer.metadata.lossy_replacement_count) +
                   " replacement(s) (U+FFFD); original encoding could not be fully determined; editing is disabled for this lossy read.]";
    }

    content += format_read_metadata_footer(metadata);

    ToolSummary summary;
    summary.verb = "Read";
    summary.object = file_path;
    summary.metrics.emplace_back("lines", std::to_string(displayed_line_count));
    summary.metrics.emplace_back("size", format_bytes_compact(content.size()));
    summary.metrics.emplace_back("enc", metadata.encoding);
    summary.metrics.emplace_back("eol", metadata.line_ending);
    if (buffer.metadata.lossy) {
        summary.metrics.emplace_back("lossy",
                                     std::to_string(buffer.metadata.lossy_replacement_count));
    }
    if (hint_added) summary.metrics.emplace_back("hint", "large_file");
    summary.icon = tool_icon("file_read");

    ToolResult r{content, true};
    r.summary = std::move(summary);
    return r;
}

ToolImpl create_file_read_tool() {
    ToolDef def;
    def.name = "file_read";
    def.description = "Read the contents of a file. Use start_line/end_line for partial reads. "
                      "Do not re-read the same file/range if its contents are already current in the conversation; repeated unchanged reads return a compact stub. "
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
