#include "diff_utils.hpp"
#include <sstream>
#include <vector>

namespace acecode {

static std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> lines;
    std::istringstream iss(s);
    std::string line;
    while (std::getline(iss, line)) {
        lines.push_back(line);
    }
    return lines;
}

std::string generate_unified_diff(
    const std::string& old_content,
    const std::string& new_content,
    const std::string& file_path
) {
    auto old_lines = split_lines(old_content);
    auto new_lines = split_lines(new_content);

    std::ostringstream out;
    out << "--- " << file_path << "\n";
    out << "+++ " << file_path << "\n";

    // Simple LCS-based diff: find common prefix and suffix, emit one hunk
    size_t common_prefix = 0;
    while (common_prefix < old_lines.size() && common_prefix < new_lines.size() &&
           old_lines[common_prefix] == new_lines[common_prefix]) {
        common_prefix++;
    }

    size_t common_suffix = 0;
    while (common_suffix < (old_lines.size() - common_prefix) &&
           common_suffix < (new_lines.size() - common_prefix) &&
           old_lines[old_lines.size() - 1 - common_suffix] == new_lines[new_lines.size() - 1 - common_suffix]) {
        common_suffix++;
    }

    // Context lines around the change
    size_t ctx = 3;
    size_t hunk_start_old = common_prefix > ctx ? common_prefix - ctx : 0;
    size_t hunk_end_old = (old_lines.size() - common_suffix + ctx < old_lines.size())
                              ? old_lines.size() - common_suffix + ctx
                              : old_lines.size();
    size_t hunk_start_new = hunk_start_old; // same start due to our approach
    size_t changed_old_start = common_prefix;
    size_t changed_old_end = old_lines.size() - common_suffix;
    size_t changed_new_start = common_prefix;
    size_t changed_new_end = new_lines.size() - common_suffix;

    size_t hunk_end_new = changed_new_end + ctx;
    if (hunk_end_new > new_lines.size()) hunk_end_new = new_lines.size();

    size_t old_count = hunk_end_old - hunk_start_old;
    size_t new_count = hunk_end_new - hunk_start_new;

    out << "@@ -" << (hunk_start_old + 1) << "," << old_count
        << " +" << (hunk_start_new + 1) << "," << new_count << " @@\n";

    // Context before
    for (size_t i = hunk_start_old; i < changed_old_start; i++) {
        out << " " << old_lines[i] << "\n";
    }
    // Removed lines
    for (size_t i = changed_old_start; i < changed_old_end; i++) {
        out << "-" << old_lines[i] << "\n";
    }
    // Added lines
    for (size_t i = changed_new_start; i < changed_new_end; i++) {
        out << "+" << new_lines[i] << "\n";
    }
    // Context after
    for (size_t i = changed_old_end; i < hunk_end_old; i++) {
        out << " " << old_lines[i] << "\n";
    }

    return out.str();
}

} // namespace acecode
