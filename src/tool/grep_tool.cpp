#include "grep_tool.hpp"
#include "ignore_utils.hpp"
#include "utils/encoding.hpp"
#include "utils/utf8_path.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <regex>
#include <algorithm>

namespace acecode {

static constexpr size_t MAX_RESULTS = 200;
static constexpr uintmax_t MAX_FILE_SIZE = 1024 * 1024;

// 用户点「停止」后工具必须尽快返回:大仓库 + 慢盘上的全量递归 grep 实测可阻塞
// 30 秒以上,期间 abort 只能干等(2026-07-11 日志复盘,同 McpManager::invoke
// 的教训)。遍历与逐行匹配循环都以此文案立即退出。
static constexpr const char* ABORTED_MSG =
    "[Aborted] Search abandoned because the user aborted the turn.";

static bool abort_requested(const ToolContext& ctx) {
    return ctx.abort_flag && ctx.abort_flag->load();
}

// Simple glob match for include_pattern (e.g. "*.cpp", "*.{hpp,cpp}")
static bool filename_matches(const std::string& filename, const std::string& pattern) {
    if (pattern.empty()) return true;
    // Simple wildcard: *.ext
    if (pattern.size() >= 2 && pattern[0] == '*' && pattern[1] == '.') {
        std::string ext = pattern.substr(1); // ".ext"
        if (filename.size() >= ext.size() &&
            filename.compare(filename.size() - ext.size(), ext.size(), ext) == 0) {
            return true;
        }
        return false;
    }
    return filename == pattern;
}

static bool grep_file(const std::filesystem::path& file_path,
                      const std::filesystem::path& display_root,
                      const std::regex& re,
                      std::ostringstream& results,
                      size_t& match_count,
                      bool& truncated,
                      const ToolContext& ctx,
                      bool& aborted) {
    std::ifstream ifs(file_path, std::ios::binary);
    if (!ifs.is_open()) return false;

    std::error_code ec;
    std::string display_path;
    if (!display_root.empty()) {
        auto rel = std::filesystem::relative(file_path, display_root, ec);
        display_path = ec ? path_to_utf8(file_path) : path_to_utf8_generic(rel);
    } else {
        display_path = path_to_utf8(file_path.filename());
    }

    std::string line;
    int line_num = 0;
    while (std::getline(ifs, line)) {
        if (abort_requested(ctx)) {
            aborted = true;
            return true;
        }
        line_num++;
        if (std::regex_search(line, re)) {
            results << display_path << ":" << line_num << ":" << ensure_utf8(line) << "\n";
            match_count++;

            if (match_count >= MAX_RESULTS) {
                truncated = true;
                break;
            }
        }
    }

    return true;
}

static ToolResult execute_grep(const std::string& arguments_json, const ToolContext& ctx) {
    std::string pattern;
    std::string include_pattern;
    std::string search_path;

    try {
        auto args = nlohmann::json::parse(arguments_json);
        pattern = args.value("pattern", "");
        include_pattern = args.value("include_pattern", "");
        search_path = args.value("path", "");
    } catch (...) {
        return ToolResult{"[Error] Failed to parse tool arguments.", false};
    }

    if (pattern.empty()) {
        return ToolResult{"[Error] No pattern provided.", false};
    }

    if (search_path.empty()) {
        search_path = ctx.cwd.empty() ? current_path_utf8() : ctx.cwd;
    }

    const auto search_root = path_from_utf8(search_path);
    std::error_code path_ec;
    const bool path_is_directory = std::filesystem::is_directory(search_root, path_ec);
    path_ec.clear();
    const bool path_is_file = std::filesystem::is_regular_file(search_root, path_ec);

    if (!path_is_directory && !path_is_file) {
        return ToolResult{"[Error] Path is not a file or directory: " + search_path, false};
    }

    std::regex re;
    try {
        re = std::regex(pattern, std::regex_constants::ECMAScript | std::regex_constants::icase);
    } catch (const std::regex_error& e) {
        return ToolResult{"[Error] Invalid regex pattern: " + std::string(e.what()), false};
    }

    std::ostringstream results;
    size_t match_count = 0;
    bool truncated = false;
    bool aborted = false;

    if (path_is_file) {
        std::error_code size_ec;
        const auto size = std::filesystem::file_size(search_root, size_ec);
        if (!size_ec && size > MAX_FILE_SIZE) {
            return ToolResult{"[Error] File is too large to grep (>1MB): " + search_path, false};
        }

        if (!grep_file(search_root, search_root.parent_path(), re, results, match_count, truncated,
                       ctx, aborted)) {
            return ToolResult{"[Error] Failed to read file: " + search_path, false};
        }
        if (aborted) return ToolResult{ABORTED_MSG, false};
    } else {
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(
                 search_root,
                 std::filesystem::directory_options::skip_permission_denied,
                 ec);
             it != std::filesystem::recursive_directory_iterator(); ++it)
        {
            if (abort_requested(ctx)) return ToolResult{ABORTED_MSG, false};
            if (ec) { ec.clear(); continue; }

            if (it->is_directory()) {
                if (should_ignore_dir(path_to_utf8(it->path().filename()))) {
                    it.disable_recursion_pending();
                    continue;
                }
                continue;
            }

            if (!it->is_regular_file()) continue;

            // Check include pattern
            if (!include_pattern.empty() &&
                !filename_matches(path_to_utf8(it->path().filename()), include_pattern)) {
                continue;
            }

            // Skip large files
            if (it->file_size() > MAX_FILE_SIZE) continue; // >1MB skip

            grep_file(it->path(), search_root, re, results, match_count, truncated, ctx, aborted);
            if (aborted) return ToolResult{ABORTED_MSG, false};
            if (truncated) break;
        }
    }

    if (match_count == 0) {
        return ToolResult{"No matches found for pattern: " + pattern, true};
    }

    std::string output = results.str();
    if (truncated) {
        output += "\n[Results truncated at " + std::to_string(MAX_RESULTS) +
            " matches. Narrow your search pattern.]";
    }

    return ToolResult{output, true};
}

ToolImpl create_grep_tool() {
    ToolDef def;
    def.name = "grep";
    def.description = "Search for a regex pattern in file contents. "
                      "If path is a directory, searches recursively. "
                      "If path is a file, searches only that file. "
                      "Returns matching lines with file path and line number. "
                      "Skips .git, node_modules, build directories.";
    def.parameters = nlohmann::json({
        {"type", "object"},
        {"properties", {
            {"pattern", {
                {"type", "string"},
                {"description", "Regex pattern to search for (case-insensitive)"}
            }},
            {"include_pattern", {
                {"type", "string"},
                {"description", "Only search files matching this pattern when path is a directory (e.g. *.cpp). Optional."}
            }},
            {"path", {
                {"type", "string"},
                {"description", "File or directory to search in (default: CWD). Directory paths search recursively; file paths search only that file. Optional."}
            }}
        }},
        {"required", nlohmann::json::array({"pattern"})}
    });

    return ToolImpl{def, execute_grep, /*is_read_only=*/true};
}

} // namespace acecode
