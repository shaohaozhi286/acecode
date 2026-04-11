#include "grep_tool.hpp"
#include "ignore_utils.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <regex>
#include <algorithm>

namespace acecode {

static constexpr size_t MAX_RESULTS = 200;

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

static ToolResult execute_grep(const std::string& arguments_json) {
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
        search_path = std::filesystem::current_path().string();
    }

    if (!std::filesystem::is_directory(search_path)) {
        return ToolResult{"[Error] Path is not a directory: " + search_path, false};
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

    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(
             search_path,
             std::filesystem::directory_options::skip_permission_denied,
             ec);
         it != std::filesystem::recursive_directory_iterator(); ++it)
    {
        if (ec) { ec.clear(); continue; }

        if (it->is_directory()) {
            if (should_ignore_dir(it->path().filename().string())) {
                it.disable_recursion_pending();
                continue;
            }
            continue;
        }

        if (!it->is_regular_file()) continue;

        // Check include pattern
        if (!include_pattern.empty() &&
            !filename_matches(it->path().filename().string(), include_pattern)) {
            continue;
        }

        // Skip large files
        if (it->file_size() > 1024 * 1024) continue; // >1MB skip

        std::ifstream ifs(it->path(), std::ios::binary);
        if (!ifs.is_open()) continue;

        std::string line;
        int line_num = 0;
        while (std::getline(ifs, line)) {
            line_num++;
            if (std::regex_search(line, re)) {
                // Use relative path for cleaner output
                auto rel = std::filesystem::relative(it->path(), search_path, ec);
                std::string display_path = ec ? it->path().string() : rel.string();

                results << display_path << ":" << line_num << ":" << line << "\n";
                match_count++;

                if (match_count >= MAX_RESULTS) {
                    truncated = true;
                    break;
                }
            }
        }
        if (truncated) break;
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
    def.description = "Search for a regex pattern in file contents recursively. "
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
                {"description", "Only search files matching this pattern (e.g. *.cpp). Optional."}
            }},
            {"path", {
                {"type", "string"},
                {"description", "Directory to search in (default: CWD). Optional."}
            }}
        }},
        {"required", nlohmann::json::array({"pattern"})}
    });

    return ToolImpl{def, execute_grep};
}

} // namespace acecode
