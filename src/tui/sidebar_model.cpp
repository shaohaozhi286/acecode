#include "sidebar_model.hpp"

#include "../utils/utf8_path.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <cstdlib>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>

namespace acecode::tui {

namespace {

int parse_metric_value(const std::string& value) {
    const char* begin = value.c_str();
    char* end = nullptr;
    long parsed = std::strtol(begin, &end, 10);
    if (end == begin) {
        return 0;
    }
    while (*end != '\0') {
        if (!std::isspace(static_cast<unsigned char>(*end))) {
            return 0;
        }
        ++end;
    }
    parsed = std::max<long>(0, parsed);
    parsed = std::min<long>(parsed, std::numeric_limits<int>::max());
    return static_cast<int>(parsed);
}

int metric_value(const ToolSummary& summary, const std::string& label) {
    for (const auto& metric : summary.metrics) {
        if (metric.first == label) {
            return parse_metric_value(metric.second);
        }
    }
    return 0;
}

bool relative_path_stays_inside(const std::filesystem::path& rel) {
    if (rel.empty() || rel.is_absolute()) {
        return false;
    }
    for (const auto& part : rel) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

std::string dot_relative_display(const std::filesystem::path& rel) {
    std::filesystem::path normalized = rel.lexically_normal();
    if (normalized.empty() || normalized == ".") {
        return ".";
    }

    std::string text = path_to_utf8(normalized);
    while (!text.empty() && (text.front() == '/' || text.front() == '\\')) {
        text.erase(text.begin());
    }
    if (text.empty()) {
        return ".";
    }
#ifdef _WIN32
    constexpr char separator = '\\';
#else
    constexpr char separator = '/';
#endif
    return std::string(".") + separator + text;
}

std::string display_path_for_file(const std::string& file,
                                  const std::string& workspace_cwd) {
    std::filesystem::path target = path_from_utf8(file).lexically_normal();
    if (target.empty()) {
        return file;
    }
    if (target.is_relative()) {
        return dot_relative_display(target);
    }
    if (workspace_cwd.empty()) {
        return file;
    }

    std::filesystem::path root = path_from_utf8(workspace_cwd).lexically_normal();
    if (root.empty()) {
        return file;
    }
    std::filesystem::path rel = target.lexically_relative(root);
    if (relative_path_stays_inside(rel)) {
        return dot_relative_display(rel);
    }
    return file;
}

} // namespace

std::vector<SidebarFileChange> collect_sidebar_file_changes(
    const std::vector<TuiState::Message>& messages,
    const std::string& workspace_cwd) {
    std::vector<SidebarFileChange> changes;
    std::unordered_map<std::string, std::size_t> index_by_file;

    for (const auto& message : messages) {
        if (message.role != "tool_result" ||
            !message.hunks.has_value() ||
            message.hunks->empty() ||
            !message.summary.has_value() ||
            message.summary->object.empty()) {
            continue;
        }

        const std::string& file = message.summary->object;
        auto it = index_by_file.find(file);
        if (it == index_by_file.end()) {
            it = index_by_file.emplace(file, changes.size()).first;
            SidebarFileChange change;
            change.file = file;
            change.display_file = display_path_for_file(file, workspace_cwd);
            changes.push_back(std::move(change));
        }

        SidebarFileChange& change = changes[it->second];
        change.additions += metric_value(*message.summary, "+");
        change.deletions += metric_value(*message.summary, "-");
    }

    return changes;
}

} // namespace acecode::tui
