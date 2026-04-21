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

std::vector<DiffHunk> generate_structured_diff(
    const std::string& old_content,
    const std::string& new_content,
    const std::string& /*file_path*/
) {
    auto old_lines = split_lines(old_content);
    auto new_lines = split_lines(new_content);

    // "Simple LCS-ish diff": 剥公共前缀 + 公共后缀,中间当作整段替换。
    size_t common_prefix = 0;
    while (common_prefix < old_lines.size() && common_prefix < new_lines.size() &&
           old_lines[common_prefix] == new_lines[common_prefix]) {
        common_prefix++;
    }

    size_t common_suffix = 0;
    while (common_suffix < (old_lines.size() - common_prefix) &&
           common_suffix < (new_lines.size() - common_prefix) &&
           old_lines[old_lines.size() - 1 - common_suffix] ==
               new_lines[new_lines.size() - 1 - common_suffix]) {
        common_suffix++;
    }

    size_t changed_old_start = common_prefix;
    size_t changed_old_end = old_lines.size() - common_suffix;
    size_t changed_new_start = common_prefix;
    size_t changed_new_end = new_lines.size() - common_suffix;

    // 无改动:直接空向量。
    if (changed_old_start >= changed_old_end && changed_new_start >= changed_new_end) {
        return {};
    }

    const size_t ctx = 3;
    size_t hunk_start_old = (common_prefix > ctx) ? (common_prefix - ctx) : 0;
    size_t hunk_end_old = changed_old_end + ctx;
    if (hunk_end_old > old_lines.size()) hunk_end_old = old_lines.size();
    size_t hunk_start_new = hunk_start_old; // 公共前缀使两侧起点同步
    size_t hunk_end_new = changed_new_end + ctx;
    if (hunk_end_new > new_lines.size()) hunk_end_new = new_lines.size();

    DiffHunk hunk;
    hunk.old_start = static_cast<int>(hunk_start_old + 1);
    hunk.new_start = static_cast<int>(hunk_start_new + 1);
    hunk.old_count = static_cast<int>(hunk_end_old - hunk_start_old);
    hunk.new_count = static_cast<int>(hunk_end_new - hunk_start_new);

    int next_old_no = hunk.old_start;
    int next_new_no = hunk.new_start;

    // Context before
    for (size_t i = hunk_start_old; i < changed_old_start; ++i) {
        DiffLine l;
        l.kind = DiffLineKind::Context;
        l.text = old_lines[i];
        l.old_line_no = next_old_no++;
        l.new_line_no = next_new_no++;
        hunk.lines.push_back(std::move(l));
    }
    // Removed
    for (size_t i = changed_old_start; i < changed_old_end; ++i) {
        DiffLine l;
        l.kind = DiffLineKind::Removed;
        l.text = old_lines[i];
        l.old_line_no = next_old_no++;
        // new_line_no 对 Removed 保持空
        hunk.lines.push_back(std::move(l));
    }
    // Added
    for (size_t i = changed_new_start; i < changed_new_end; ++i) {
        DiffLine l;
        l.kind = DiffLineKind::Added;
        l.text = new_lines[i];
        // old_line_no 对 Added 保持空
        l.new_line_no = next_new_no++;
        hunk.lines.push_back(std::move(l));
    }
    // Context after
    for (size_t i = changed_old_end; i < hunk_end_old; ++i) {
        DiffLine l;
        l.kind = DiffLineKind::Context;
        l.text = old_lines[i];
        l.old_line_no = next_old_no++;
        l.new_line_no = next_new_no++;
        hunk.lines.push_back(std::move(l));
    }

    return {std::move(hunk)};
}

std::string generate_unified_diff(
    const std::string& old_content,
    const std::string& new_content,
    const std::string& file_path,
    DiffStats& stats
) {
    stats.additions = 0;
    stats.deletions = 0;

    auto hunks = generate_structured_diff(old_content, new_content, file_path);

    std::ostringstream out;
    out << "--- " << file_path << "\n";
    out << "+++ " << file_path << "\n";

    if (hunks.empty()) {
        // 保持和旧实现的输出格式兼容:虽然没有 hunk,但标头仍输出。
        return out.str();
    }

    for (const auto& hunk : hunks) {
        out << "@@ -" << hunk.old_start << "," << hunk.old_count
            << " +" << hunk.new_start << "," << hunk.new_count << " @@\n";
        for (const auto& line : hunk.lines) {
            char marker = ' ';
            switch (line.kind) {
                case DiffLineKind::Added:
                    marker = '+';
                    ++stats.additions;
                    break;
                case DiffLineKind::Removed:
                    marker = '-';
                    ++stats.deletions;
                    break;
                case DiffLineKind::Context:
                    marker = ' ';
                    break;
            }
            out << marker << line.text << "\n";
        }
    }

    return out.str();
}

std::string generate_unified_diff(
    const std::string& old_content,
    const std::string& new_content,
    const std::string& file_path
) {
    DiffStats discard;
    return generate_unified_diff(old_content, new_content, file_path, discard);
}

} // namespace acecode
