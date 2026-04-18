#include "glob_tool.hpp"
#include "ignore_utils.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <sstream>
#include <vector>
#include <algorithm>

namespace acecode {

static constexpr size_t MAX_GLOB_RESULTS = 500;

// Simple glob pattern matcher supporting *, **, and ?
static bool glob_match(const std::string& pattern, const std::string& path) {
    // Split pattern and path into segments by '/' or '\'
    auto split = [](const std::string& s) {
        std::vector<std::string> parts;
        std::string part;
        for (char c : s) {
            if (c == '/' || c == '\\') {
                if (!part.empty()) {
                    parts.push_back(part);
                    part.clear();
                }
            } else {
                part += c;
            }
        }
        if (!part.empty()) parts.push_back(part);
        return parts;
    };

    // Match a single segment pattern (with * and ?) against a string
    std::function<bool(const char*, const char*)> match_segment;
    match_segment = [&](const char* p, const char* s) -> bool {
        while (*p && *s) {
            if (*p == '*') {
                p++;
                // '*' matches any sequence within a segment
                while (*s) {
                    if (match_segment(p, s)) return true;
                    s++;
                }
                return match_segment(p, s);
            }
            if (*p == '?') {
                p++;
                s++;
                continue;
            }
            if (*p != *s) return false;
            p++;
            s++;
        }
        while (*p == '*') p++;
        return *p == 0 && *s == 0;
    };

    auto pat_parts = split(pattern);
    auto path_parts = split(path);

    // Recursive match with ** support
    std::function<bool(size_t, size_t)> match_parts;
    match_parts = [&](size_t pi, size_t si) -> bool {
        if (pi == pat_parts.size() && si == path_parts.size()) return true;
        if (pi == pat_parts.size()) return false;

        if (pat_parts[pi] == "**") {
            // ** matches zero or more directories
            for (size_t i = si; i <= path_parts.size(); i++) {
                if (match_parts(pi + 1, i)) return true;
            }
            return false;
        }

        if (si == path_parts.size()) return false;

        if (match_segment(pat_parts[pi].c_str(), path_parts[si].c_str())) {
            return match_parts(pi + 1, si + 1);
        }
        return false;
    };

    return match_parts(0, 0);
}

static ToolResult execute_glob(const std::string& arguments_json, const ToolContext& /*ctx*/) {
    std::string pattern;
    std::string search_path;

    try {
        auto args = nlohmann::json::parse(arguments_json);
        pattern = args.value("pattern", "");
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

    std::vector<std::string> matches;
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
            }
            continue;
        }

        if (!it->is_regular_file()) continue;

        auto rel = std::filesystem::relative(it->path(), search_path, ec);
        if (ec) { ec.clear(); continue; }

        std::string rel_str = rel.generic_string(); // use forward slashes
        if (glob_match(pattern, rel_str)) {
            matches.push_back(rel_str);
            if (matches.size() >= MAX_GLOB_RESULTS) {
                truncated = true;
                break;
            }
        }
    }

    if (matches.empty()) {
        return ToolResult{"No files found matching pattern: " + pattern, true};
    }

    std::sort(matches.begin(), matches.end());

    std::ostringstream out;
    for (const auto& m : matches) {
        out << m << "\n";
    }

    if (truncated) {
        out << "\n[Results truncated at " << MAX_GLOB_RESULTS
            << " files. Narrow your pattern.]";
    }

    return ToolResult{out.str(), true};
}

ToolImpl create_glob_tool() {
    ToolDef def;
    def.name = "glob";
    def.description = "Find files matching a glob pattern. Supports *, **, and ?. "
                      "Use to discover files in a project. "
                      "Skips .git, node_modules, build directories.";
    def.parameters = nlohmann::json({
        {"type", "object"},
        {"properties", {
            {"pattern", {
                {"type", "string"},
                {"description", "Glob pattern (e.g. 'src/**/*.cpp', '*.hpp', '**/*.json')"}
            }},
            {"path", {
                {"type", "string"},
                {"description", "Directory to search in (default: CWD). Optional."}
            }}
        }},
        {"required", nlohmann::json::array({"pattern"})}
    });

    return ToolImpl{def, execute_glob, /*is_read_only=*/true};
}

} // namespace acecode
