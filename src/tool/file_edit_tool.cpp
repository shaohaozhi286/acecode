#include "file_edit_tool.hpp"
#include "mtime_tracker.hpp"
#include "diff_utils.hpp"
#include "tool_icons.hpp"
#include "utils/logger.hpp"
#include "utils/tool_args_parser.hpp"
#include "utils/tool_errors.hpp"
#include "utils/file_operations.hpp"
#include "utils/text_file_buffer.hpp"
#include "utils/utf8_path.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <optional>
#include <sstream>
#include <vector>

namespace acecode {

namespace {

static bool ends_with_ipynb(const std::string& path) {
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    const std::string suffix = ".ipynb";
    return lower.size() >= suffix.size() &&
           lower.compare(lower.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static bool is_blank_content(const std::string& content) {
    for (unsigned char c : content) {
        if (!std::isspace(c)) return false;
    }
    return true;
}

static std::string ascii_lower_trim(std::string s) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && is_space(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && is_space(static_cast<unsigned char>(s.back()))) s.pop_back();
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

static bool parse_semantic_bool(const std::string& arguments_json, const std::string& key, bool default_value) {
    auto j = nlohmann::json::parse(arguments_json, nullptr, false);
    if (j.is_discarded() || !j.contains(key)) return default_value;

    const auto& v = j[key];
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number_integer()) return v.get<int>() != 0;
    if (v.is_string()) {
        const std::string value = ascii_lower_trim(v.get<std::string>());
        if (value == "true" || value == "1" || value == "yes" || value == "on") return true;
        if (value == "false" || value == "0" || value == "no" || value == "off") return false;
    }
    return default_value;
}

static bool file_uses_crlf(const std::string& content) {
    return content.find("\r\n") != std::string::npos;
}

static std::string lf_to_crlf(std::string value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\n' && (i == 0 || value[i - 1] != '\r')) {
            out += "\r\n";
        } else {
            out += value[i];
        }
    }
    return out;
}

struct QuoteNormalizedText {
    std::string text;
    std::vector<size_t> start_offsets;
    std::vector<size_t> end_offsets;
};

static bool starts_with_at(const std::string& value, size_t pos, const std::string& needle) {
    return pos + needle.size() <= value.size() &&
           value.compare(pos, needle.size(), needle) == 0;
}

static QuoteNormalizedText normalize_quotes_with_offsets(const std::string& value) {
    static const std::string left_single = u8"‘";
    static const std::string right_single = u8"’";
    static const std::string left_double = u8"“";
    static const std::string right_double = u8"”";

    QuoteNormalizedText out;
    out.text.reserve(value.size());
    out.start_offsets.reserve(value.size());
    out.end_offsets.reserve(value.size());

    for (size_t i = 0; i < value.size();) {
        std::string replacement;
        size_t next = i + 1;
        if (starts_with_at(value, i, left_single) || starts_with_at(value, i, right_single)) {
            replacement = "'";
            next = i + left_single.size();
        } else if (starts_with_at(value, i, left_double) || starts_with_at(value, i, right_double)) {
            replacement = "\"";
            next = i + left_double.size();
        } else {
            replacement.assign(1, value[i]);
        }

        for (char c : replacement) {
            out.text.push_back(c);
            out.start_offsets.push_back(i);
            out.end_offsets.push_back(next);
        }
        i = next;
    }

    return out;
}

static std::string normalize_quotes(const std::string& value) {
    return normalize_quotes_with_offsets(value).text;
}

static std::optional<std::string> find_actual_string_by_quotes(
    const std::string& content,
    const std::string& search
) {
    if (search.empty()) return std::nullopt;

    auto normalized_content = normalize_quotes_with_offsets(content);
    const std::string normalized_search = normalize_quotes(search);
    const size_t found = normalized_content.text.find(normalized_search);
    if (found == std::string::npos || normalized_search.empty()) {
        return std::nullopt;
    }

    const size_t last = found + normalized_search.size() - 1;
    if (found >= normalized_content.start_offsets.size() ||
        last >= normalized_content.end_offsets.size()) {
        return std::nullopt;
    }

    const size_t start = normalized_content.start_offsets[found];
    const size_t end = normalized_content.end_offsets[last];
    if (end < start || end > content.size()) return std::nullopt;
    return content.substr(start, end - start);
}

static bool is_ascii_letter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool is_opening_quote_context(const std::string& value, size_t index) {
    if (index == 0) return true;
    const char prev = value[index - 1];
    return prev == ' ' || prev == '\t' || prev == '\n' || prev == '\r' ||
           prev == '(' || prev == '[' || prev == '{';
}

static std::string apply_curly_double_quotes(const std::string& value) {
    static const std::string left_double = u8"“";
    static const std::string right_double = u8"”";

    std::string out;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '"') {
            out += is_opening_quote_context(value, i) ? left_double : right_double;
        } else {
            out.push_back(value[i]);
        }
    }
    return out;
}

static std::string apply_curly_single_quotes(const std::string& value) {
    static const std::string left_single = u8"‘";
    static const std::string right_single = u8"’";

    std::string out;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\'') {
            const bool contraction =
                i > 0 &&
                i + 1 < value.size() &&
                is_ascii_letter(value[i - 1]) &&
                is_ascii_letter(value[i + 1]);
            if (contraction) {
                out += right_single;
            } else {
                out += is_opening_quote_context(value, i) ? left_single : right_single;
            }
        } else {
            out.push_back(value[i]);
        }
    }
    return out;
}

static std::string preserve_quote_style(
    const std::string& requested_old,
    const std::string& actual_old,
    const std::string& requested_new
) {
    if (requested_old == actual_old) return requested_new;

    static const std::string left_single = u8"‘";
    static const std::string right_single = u8"’";
    static const std::string left_double = u8"“";
    static const std::string right_double = u8"”";

    std::string result = requested_new;
    if (actual_old.find(left_double) != std::string::npos ||
        actual_old.find(right_double) != std::string::npos) {
        result = apply_curly_double_quotes(result);
    }
    if (actual_old.find(left_single) != std::string::npos ||
        actual_old.find(right_single) != std::string::npos) {
        result = apply_curly_single_quotes(result);
    }
    return result;
}

struct MatchPlan {
    std::string actual_old;
    std::string actual_new;
};

static std::optional<MatchPlan> build_match_plan(
    const std::string& content,
    const std::string& requested_old,
    const std::string& requested_new
) {
    if (content.find(requested_old) != std::string::npos) {
        return MatchPlan{requested_old, requested_new};
    }

    // 模型常按 LF 组织多行字符串；CRLF 文件要在匹配和替换两端同步适配。
    if (file_uses_crlf(content) && requested_old.find('\n') != std::string::npos) {
        const std::string old_crlf = lf_to_crlf(requested_old);
        const std::string new_crlf = lf_to_crlf(requested_new);
        if (content.find(old_crlf) != std::string::npos) {
            return MatchPlan{old_crlf, new_crlf};
        }
        if (auto actual = find_actual_string_by_quotes(content, old_crlf)) {
            return MatchPlan{*actual, preserve_quote_style(old_crlf, *actual, new_crlf)};
        }
    }

    // ClaudeCode 的 Edit 允许 ASCII 引号命中 curly quote；这里保持同样的宽容度,
    // 但最终仍替换文件中的真实字节范围。
    if (auto actual = find_actual_string_by_quotes(content, requested_old)) {
        return MatchPlan{*actual, preserve_quote_style(requested_old, *actual, requested_new)};
    }

    return std::nullopt;
}

static size_t count_occurrences(const std::string& content, const std::string& needle) {
    if (needle.empty()) return 0;
    size_t count = 0;
    size_t pos = 0;
    while ((pos = content.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

static std::string replace_occurrences(
    const std::string& content,
    const std::string& needle,
    const std::string& replacement,
    bool replace_all
) {
    const size_t first = content.find(needle);
    if (first == std::string::npos) return content;

    if (!replace_all) {
        std::string out = content;
        out.replace(first, needle.size(), replacement);
        return out;
    }

    std::string out;
    out.reserve(content.size());
    size_t pos = 0;
    size_t found = 0;
    while ((found = content.find(needle, pos)) != std::string::npos) {
        out.append(content, pos, found - pos);
        out += replacement;
        pos = found + needle.size();
    }
    out.append(content, pos, std::string::npos);
    return out;
}

static ToolResult run_validated_write(
    const std::string& file_path,
    bool file_existed,
    const std::string& old_content,
    const std::string& new_content,
    const TextFileMetadata& metadata,
    const ToolContext& ctx
) {
    auto before_write = [&](const std::string& path) {
        if (ctx.track_file_write_before) {
            try {
                ctx.track_file_write_before(path);
            } catch (const std::exception& e) {
                LOG_WARN(std::string("file_edit checkpoint hook failed: ") + e.what());
            } catch (...) {
                LOG_WARN("file_edit checkpoint hook failed with unknown error");
            }
        }
    };

    auto write_result = safe_write_text_file(file_path, new_content, metadata, before_write);
    if (!write_result.success) {
        return ToolResult{write_result.error, false};
    }

    MtimeTracker::instance().record_write(file_path, new_content);

    // 同时产出结构化 hunk + 文本 diff,保证 TUI 彩色渲染和 LLM 下一轮阅读同源。
    DiffStats stats;
    std::string diff = generate_unified_diff(old_content, new_content, file_path, stats);
    auto structured = generate_structured_diff(old_content, new_content, file_path);

    ToolSummary summary;
    summary.verb = file_existed ? "Edited" : "Created";
    summary.object = file_path;
    summary.metrics.emplace_back("+", std::to_string(stats.additions));
    summary.metrics.emplace_back("-", std::to_string(stats.deletions));
    summary.icon = tool_icon("file_edit");

    ToolResult r{(file_existed ? "Edited " : "Created file: ") + file_path + "\n\n" + diff, true};
    r.summary = std::move(summary);
    r.hunks = std::move(structured);
    return r;
}

static std::string normalize_expected_hash(std::string hash) {
    while (!hash.empty() && std::isspace(static_cast<unsigned char>(hash.front()))) hash.erase(hash.begin());
    while (!hash.empty() && std::isspace(static_cast<unsigned char>(hash.back()))) hash.pop_back();
    if (hash.rfind("sha256:", 0) == 0) return hash;
    return "sha256:" + hash;
}

static bool find_line_range_offsets(const std::string& lf_text,
                                    int start_line,
                                    int end_line,
                                    size_t& start_offset,
                                    size_t& end_offset,
                                    int& total_lines) {
    start_offset = 0;
    end_offset = 0;
    total_lines = 0;
    if (start_line <= 0 || end_line < start_line) return false;

    std::vector<size_t> starts;
    if (!lf_text.empty()) starts.push_back(0);
    for (size_t i = 0; i < lf_text.size(); ++i) {
        if (lf_text[i] == '\n' && i + 1 < lf_text.size()) {
            starts.push_back(i + 1);
        }
    }
    total_lines = static_cast<int>(starts.size());
    if (total_lines == 0 || start_line > total_lines) return false;

    const int clamped_end = std::min(end_line, total_lines);
    start_offset = starts[static_cast<size_t>(start_line - 1)];
    end_offset = clamped_end < total_lines
        ? starts[static_cast<size_t>(clamped_end)]
        : lf_text.size();
    return true;
}

static ToolResult make_hash_mismatch_result(const std::string& file_path,
                                            const std::string& content,
                                            int start_line,
                                            int end_line,
                                            const std::string& current_hash) {
    std::ostringstream oss;
    oss << "[Error] range hash mismatch in " << file_path << ". The file changed "
        << "since it was read, or the wrong range was supplied.\n"
        << "Retry with expected_hash=\"" << current_hash << "\" and this current range:\n"
        << "```text\n" << content << "```\n";
    ToolResult result{oss.str(), false};
    ToolSummary summary;
    summary.verb = "Edit failed";
    summary.object = file_path;
    summary.metrics.emplace_back("range", std::to_string(start_line) + "-" + std::to_string(end_line));
    summary.metrics.emplace_back("hash", current_hash);
    summary.icon = tool_icon("file_edit");
    result.summary = std::move(summary);
    return result;
}

static ToolResult make_range_already_applied_result(const std::string& file_path,
                                                    int start_line,
                                                    int end_line,
                                                    const std::string& current_hash) {
    std::ostringstream oss;
    oss << "Edit already applied: " << file_path << "\n"
        << "Range " << start_line << "-" << end_line
        << " already matches new_string; no write performed.";
    ToolResult result{oss.str(), true};
    ToolSummary summary;
    summary.verb = "Already applied";
    summary.object = file_path;
    summary.metrics.emplace_back("range", std::to_string(start_line) + "-" + std::to_string(end_line));
    summary.metrics.emplace_back("hash", current_hash);
    summary.metrics.emplace_back("+", "0");
    summary.metrics.emplace_back("-", "0");
    summary.icon = tool_icon("file_edit");
    result.summary = std::move(summary);
    return result;
}

static std::string normalize_range_payload(const std::string& value,
                                           const std::string& current_range) {
    std::string normalized = normalize_text_to_lf(value);
    if (!current_range.empty() && current_range.back() == '\n' &&
        !normalized.empty() && normalized.back() != '\n') {
        normalized.push_back('\n');
    }
    return normalized;
}

static ToolResult make_old_string_not_found_result(const std::string& file_path,
                                                   const std::string& normalized_content,
                                                   const std::string& requested_old) {
    std::ostringstream oss;
    oss << ToolErrors::string_not_found(file_path) << "\n"
        << "ACECode matches old_string against UTF-8 text with LF line endings. "
        << "Prefer retrying with file_read start_line/end_line metadata and "
        << "file_edit start_line/end_line/expected_hash instead of shell or Python writes.";

    std::string first_line = normalize_text_to_lf(requested_old);
    size_t nl = first_line.find('\n');
    if (nl != std::string::npos) first_line.resize(nl);
    while (!first_line.empty() && std::isspace(static_cast<unsigned char>(first_line.front()))) first_line.erase(first_line.begin());
    while (!first_line.empty() && std::isspace(static_cast<unsigned char>(first_line.back()))) first_line.pop_back();

    if (!first_line.empty()) {
        size_t pos = normalized_content.find(first_line);
        if (pos != std::string::npos) {
            int line = 1;
            for (size_t i = 0; i < pos; ++i) {
                if (normalized_content[i] == '\n') ++line;
            }
            oss << "\nA substring from old_string appears near line " << line
                << ". Re-read a narrow range around that line and use the returned range_hash.";
        }
    }
    return ToolResult{oss.str(), false};
}

} // namespace

static ToolResult execute_file_edit(const std::string& arguments_json, const ToolContext& ctx) {
    // Parse arguments
    ToolArgsParser parser(arguments_json);
    if (parser.has_error()) {
        return ToolResult{parser.error(), false};
    }

    std::string file_path = parser.get_or<std::string>("file_path", "");
    std::string old_string = parser.get_or<std::string>("old_string", "");
    std::string new_string = parser.get_or<std::string>("new_string", "");
    bool replace_all = parse_semantic_bool(arguments_json, "replace_all", false);
    int start_line = parser.get_or<int>("start_line", 0);
    int end_line = parser.get_or<int>("end_line", 0);
    std::string expected_hash = parser.get_or<std::string>("expected_hash", "");
    std::string read_id = parser.get_or<std::string>("read_id", "");
    const bool range_mode = start_line > 0 || end_line > 0 ||
                            !expected_hash.empty() || !read_id.empty();

    if (file_path.empty()) {
        return ToolResult{ToolErrors::missing_parameter("file_path"), false};
    }
    if (!range_mode && old_string == new_string) {
        return ToolResult{ToolErrors::no_changes_to_make(), false};
    }
    if (ends_with_ipynb(file_path)) {
        return ToolResult{ToolErrors::notebook_edit_required(file_path), false};
    }

    LOG_DEBUG("file_edit: path=" + file_path + " old_len=" + std::to_string(old_string.size()) +
              " new_len=" + std::to_string(new_string.size()) +
              " replace_all=" + (replace_all ? "true" : "false"));

    const bool file_exists = std::filesystem::exists(path_from_utf8(file_path));

    if (!file_exists && !old_string.empty()) {
        return ToolResult{ToolErrors::file_not_found(file_path, current_path_utf8()) +
                          ". Use file_edit with empty old_string or file_write to create a new file.",
                          false};
    }

    if (file_exists) {
        auto size_check = FileOperations::check_file_size(file_path,
            "Use file_read with start_line/end_line to inspect the file before deciding how to edit it.");
        if (!size_check.success) {
            return size_check;
        }
    }

    TextFileBuffer buffer;
    if (file_exists) {
        auto read_result = read_text_file_buffer(file_path);
        if (!read_result.success) {
            return ToolResult{read_result.error, false};
        }
        buffer = std::move(read_result.buffer);
    } else {
        buffer.path = file_path;
        buffer.metadata = default_new_file_text_metadata();
    }

    if (range_mode) {
        if (!file_exists) {
            return ToolResult{ToolErrors::file_not_found(file_path, current_path_utf8()), false};
        }
        if (start_line <= 0 || end_line < start_line || expected_hash.empty()) {
            return ToolResult{"[Error] Range edit requires start_line, end_line, and expected_hash.", false};
        }

        size_t start_offset = 0;
        size_t end_offset = 0;
        int total_lines = 0;
        if (!find_line_range_offsets(buffer.text, start_line, end_line,
                                     start_offset, end_offset, total_lines)) {
            return ToolResult{ToolErrors::no_lines_in_range(start_line, end_line, total_lines), false};
        }

        const std::string current_range = buffer.text.substr(start_offset, end_offset - start_offset);
        const std::string current_hash = range_hash(buffer.text, start_line, end_line);
        const std::string normalized_new = normalize_range_payload(new_string, current_range);
        const std::string normalized_old = normalize_range_payload(old_string, current_range);
        if (normalize_expected_hash(expected_hash) != current_hash) {
            if (current_range == normalized_new) {
                return make_range_already_applied_result(file_path, start_line, end_line, current_hash);
            }
            if (old_string.empty() || normalized_old != current_range) {
                return make_hash_mismatch_result(file_path, current_range, start_line, end_line, current_hash);
            }
            LOG_DEBUG("file_edit: stale range hash accepted because old_string matches current range");
        }

        std::string new_content = buffer.text;
        new_content.replace(start_offset, end_offset - start_offset, normalized_new);
        if (new_content == buffer.text) {
            return make_range_already_applied_result(file_path, start_line, end_line, current_hash);
        }

        return run_validated_write(file_path, true, buffer.text, new_content,
                                   buffer.metadata, ctx);
    }

    if (old_string.empty()) {
        if (file_exists && !is_blank_content(buffer.text)) {
            return ToolResult{ToolErrors::cannot_create_file_exists(file_path), false};
        }
        return run_validated_write(file_path, file_exists, buffer.text,
                                   normalize_text_to_lf(new_string),
                                   buffer.metadata, ctx);
    }

    const auto read_check = MtimeTracker::instance().validate_full_read_for_edit(file_path, buffer.text);
    switch (read_check.status) {
        case MtimeTracker::FullReadStatus::Ok:
            break;
        case MtimeTracker::FullReadStatus::NotRead:
            return ToolResult{ToolErrors::file_not_read_for_edit(file_path), false};
        case MtimeTracker::FullReadStatus::PartialRead:
            return ToolResult{ToolErrors::file_partially_read_for_edit(file_path), false};
        case MtimeTracker::FullReadStatus::ExternallyModified:
            return ToolResult{ToolErrors::external_modification(file_path), false};
    }

    std::string normalized_old = normalize_text_to_lf(old_string);
    std::string normalized_new = normalize_text_to_lf(new_string);
    auto match_plan = build_match_plan(buffer.text, normalized_old, normalized_new);
    if (!match_plan.has_value()) {
        return make_old_string_not_found_result(file_path, buffer.text, old_string);
    }

    const size_t count = count_occurrences(buffer.text, match_plan->actual_old);
    if (count == 0) {
        return make_old_string_not_found_result(file_path, buffer.text, old_string);
    }
    if (count > 1 && !replace_all) {
        return ToolResult{ToolErrors::string_not_unique(count, file_path), false};
    }

    std::string old_content = buffer.text;
    std::string new_content = replace_occurrences(
        buffer.text,
        match_plan->actual_old,
        match_plan->actual_new,
        replace_all
    );

    return run_validated_write(file_path, true, old_content, new_content,
                               buffer.metadata, ctx);
}

ToolImpl create_file_edit_tool() {
    ToolDef def;
    def.name = "file_edit";
    def.description = "Edit a file by replacing an exact string with a new string. "
                      "Read existing non-empty files with file_read before editing. "
                      "Prefer start_line/end_line/expected_hash from file_read metadata for precise range edits. "
                      "When complete range arguments are present, old_string is treated only as redundant current-range validation. "
                      "The old_string must appear exactly once unless replace_all is true. "
                      "Use empty old_string only to create a missing file or fill a blank file. "
                      "Include surrounding context lines to ensure uniqueness. "
                      "Existing text files preserve their original encoding and line endings; unsafe encoding changes are rejected. "
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
                {"description", "The exact string to find and replace. Empty string creates a missing file or fills a blank file."}
            }},
            {"new_string", {
                {"type", "string"},
                {"description", "The replacement string"}
            }},
            {"replace_all", {
                {"type", "boolean"},
                {"description", "Replace all occurrences of old_string. Defaults to false."},
                {"default", false}
            }},
            {"start_line", {
                {"type", "integer"},
                {"description", "Range edit start line (1-indexed, inclusive). Use with end_line and expected_hash instead of old_string."}
            }},
            {"end_line", {
                {"type", "integer"},
                {"description", "Range edit end line (1-indexed, inclusive). Use with start_line and expected_hash instead of old_string."}
            }},
            {"expected_hash", {
                {"type", "string"},
                {"description", "Range hash returned by file_read metadata. Required for range edits."}
            }},
            {"read_id", {
                {"type", "string"},
                {"description", "Optional read_id returned by file_read for traceability."}
            }}
        }},
        {"required", nlohmann::json::array({"file_path", "new_string"})}
    });

    return ToolImpl{def, execute_file_edit, /*is_read_only=*/false};
}

} // namespace acecode
