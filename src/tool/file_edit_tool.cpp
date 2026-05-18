#include "file_edit_tool.hpp"
#include "mtime_tracker.hpp"
#include "diff_utils.hpp"
#include "tool_icons.hpp"
#include "utils/logger.hpp"
#include "utils/tool_args_parser.hpp"
#include "utils/tool_errors.hpp"
#include "utils/file_operations.hpp"
#include "utils/utf8_path.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <optional>
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
    const ToolContext& ctx
) {
    if (ctx.track_file_write_before) {
        try {
            ctx.track_file_write_before(file_path);
        } catch (const std::exception& e) {
            LOG_WARN(std::string("file_edit checkpoint hook failed: ") + e.what());
        } catch (...) {
            LOG_WARN("file_edit checkpoint hook failed with unknown error");
        }
    }

    std::string error;
    if (!FileOperations::write_content(file_path, new_content, error)) {
        return ToolResult{error, false};
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

    if (file_path.empty()) {
        return ToolResult{ToolErrors::missing_parameter("file_path"), false};
    }
    if (old_string == new_string) {
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

    std::string content;
    std::string error;
    if (file_exists && !FileOperations::read_content(file_path, content, error)) {
        return ToolResult{error, false};
    }

    if (old_string.empty()) {
        if (file_exists && !is_blank_content(content)) {
            return ToolResult{ToolErrors::cannot_create_file_exists(file_path), false};
        }
        return run_validated_write(file_path, file_exists, content, new_string, ctx);
    }

    const auto read_check = MtimeTracker::instance().validate_full_read_for_edit(file_path, content);
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

    auto match_plan = build_match_plan(content, old_string, new_string);
    if (!match_plan.has_value()) {
        return ToolResult{ToolErrors::string_not_found(file_path), false};
    }

    const size_t count = count_occurrences(content, match_plan->actual_old);
    if (count == 0) {
        return ToolResult{ToolErrors::string_not_found(file_path), false};
    }
    if (count > 1 && !replace_all) {
        return ToolResult{ToolErrors::string_not_unique(count, file_path), false};
    }

    std::string old_content = content;
    content = replace_occurrences(
        content,
        match_plan->actual_old,
        match_plan->actual_new,
        replace_all
    );

    return run_validated_write(file_path, true, old_content, content, ctx);
}

ToolImpl create_file_edit_tool() {
    ToolDef def;
    def.name = "file_edit";
    def.description = "Edit a file by replacing an exact string with a new string. "
                      "Read existing non-empty files with file_read before editing. "
                      "The old_string must appear exactly once unless replace_all is true. "
                      "Use empty old_string only to create a missing file or fill a blank file. "
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
            }}
        }},
        {"required", nlohmann::json::array({"file_path", "old_string", "new_string"})}
    });

    return ToolImpl{def, execute_file_edit, /*is_read_only=*/false};
}

} // namespace acecode
