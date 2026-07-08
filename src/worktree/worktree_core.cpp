#include "worktree_core.hpp"

#include "../utils/utf8_path.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <random>
#include <regex>
#include <sstream>

namespace acecode::worktree {

namespace {

bool is_valid_slug_segment_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

} // namespace

std::string validate_worktree_slug(const std::string& slug) {
    if (slug.size() > kMaxWorktreeSlugLength) {
        return "Invalid worktree name: must be " +
               std::to_string(kMaxWorktreeSlugLength) +
               " characters or fewer (got " + std::to_string(slug.size()) + ")";
    }
    // 首/尾 "/" 会让 join 产生绝对路径或悬空段;按段校验时空段会被
    // 白名单正则拒绝,所以两种情况都天然覆盖,同时放行 "user/feature"。
    for (const auto& segment : split(slug, '/')) {
        if (segment == "." || segment == "..") {
            return "Invalid worktree name \"" + slug +
                   "\": must not contain \".\" or \"..\" path segments";
        }
        if (segment.empty() ||
            !std::all_of(segment.begin(), segment.end(), is_valid_slug_segment_char)) {
            return "Invalid worktree name \"" + slug +
                   "\": each \"/\"-separated segment must be non-empty and contain "
                   "only letters, digits, dots, underscores, and dashes";
        }
    }
    return "";
}

std::string flatten_slug(const std::string& slug) {
    std::string out = slug;
    std::replace(out.begin(), out.end(), '/', '+');
    return out;
}

std::string worktree_branch_name(const std::string& slug) {
    return std::string(kWorktreeBranchPrefix) + flatten_slug(slug);
}

std::string worktree_path_for(const std::string& repo_root, const std::string& slug) {
    namespace fs = std::filesystem;
    fs::path p = path_from_utf8(repo_root) / ".acecode" / "worktrees" / flatten_slug(slug);
    return path_to_utf8(p);
}

std::optional<int> parse_pr_reference(const std::string& input) {
    static const std::regex url_re(
        R"(^https?://[^/]+/[^/]+/[^/]+/pull/(\d+)/?(?:[?#].*)?$)",
        std::regex::ECMAScript | std::regex::icase);
    static const std::regex hash_re(R"(^#(\d+)$)");
    std::smatch m;
    if (std::regex_match(input, m, url_re) || std::regex_match(input, m, hash_re)) {
        try {
            return std::stoi(m[1].str());
        } catch (...) {
            return std::nullopt; // 数字溢出 int 时视为不可识别
        }
    }
    return std::nullopt;
}

std::string generate_worktree_slug(unsigned seed) {
    static const char* adjectives[] = {"swift", "bright", "calm", "keen", "bold"};
    static const char* nouns[] = {"fox", "owl", "elm", "oak", "ray"};
    static const char base36[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    std::mt19937 rng(seed);
    std::string out = adjectives[rng() % 5];
    out += '-';
    out += nouns[rng() % 5];
    out += '-';
    for (int i = 0; i < 4; ++i) out += base36[rng() % 36];
    return out;
}

// ---- .worktreeinclude -----------------------------------------------------

std::vector<std::string> parse_worktree_include_patterns(const std::string& content) {
    std::vector<std::string> out;
    std::istringstream in(content);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // 首尾空白裁掉(gitignore 的尾随空格转义是罕见角落,这里不支持)
        std::size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        std::size_t e = line.find_last_not_of(" \t");
        std::string trimmed = line.substr(b, e - b + 1);
        if (trimmed.empty() || trimmed[0] == '#') continue;
        out.push_back(trimmed);
    }
    return out;
}

namespace {

void append_escaped_regex_char(std::string& re, char c) {
    static const std::string specials = R"(\^$.|?*+()[]{})";
    if (specials.find(c) != std::string::npos) re += '\\';
    re += c;
}

// 把单个 gitignore pattern 主体(已剥离 "!" 与尾 "/")翻译成正则文本。
// anchored=false 时前缀 (?:.*/)? 让 pattern 在任意深度命中。
std::string translate_gitignore_pattern(const std::string& body, bool anchored) {
    std::string re = anchored ? "" : "(?:.*/)?";
    std::size_t i = 0;
    const std::size_t n = body.size();
    while (i < n) {
        char c = body[i];
        if (c == '*') {
            const bool double_star = (i + 1 < n && body[i + 1] == '*');
            if (double_star) {
                const bool at_start = (i == 0);
                const bool prev_slash = (!at_start && body[i - 1] == '/');
                const bool next_slash = (i + 2 < n && body[i + 2] == '/');
                const bool at_end = (i + 2 >= n);
                if ((at_start || prev_slash) && next_slash) {
                    // "**/" 段:零个或多个目录层级
                    re += "(?:[^/]+/)*";
                    i += 3;
                    continue;
                }
                if ((at_start || prev_slash) && at_end) {
                    // 尾部 "**":任意内容(含子目录)
                    re += ".*";
                    i += 2;
                    continue;
                }
                // 非段边界的 "**" 按 gitignore 语义等价于任意字符
                re += ".*";
                i += 2;
                continue;
            }
            re += "[^/]*";
            ++i;
        } else if (c == '?') {
            re += "[^/]";
            ++i;
        } else if (c == '[') {
            // 字符类透传;找不到闭合 "]" 时按字面 "[" 处理
            std::size_t close = body.find(']', i + 1);
            if (close == std::string::npos || close == i + 1 ||
                (body[i + 1] == '!' && close == i + 2)) {
                re += "\\[";
                ++i;
            } else {
                std::string cls = body.substr(i, close - i + 1);
                if (cls.size() > 1 && cls[1] == '!') cls[1] = '^';
                re += cls;
                i = close + 1;
            }
        } else {
            append_escaped_regex_char(re, c);
            ++i;
        }
    }
    return re;
}

std::string normalize_rel_path(const std::string& p) {
    std::string out = p;
    std::replace(out.begin(), out.end(), '\\', '/');
    if (out.rfind("./", 0) == 0) out = out.substr(2);
    while (!out.empty() && out.front() == '/') out.erase(out.begin());
    return out;
}

} // namespace

void WorktreeIncludeMatcher::add_pattern(const std::string& pattern) {
    std::string body = pattern;
    Rule rule;
    if (!body.empty() && body[0] == '!') {
        rule.negated = true;
        body.erase(body.begin());
    }
    if (!body.empty() && body.back() == '/') {
        rule.dir_only = true;
        body.pop_back();
    }
    if (body.empty()) return;

    // gitignore 锚定规则:首 "/" 显式锚定;主体中出现 "/"(非尾部,已剥掉)
    // 也锚定到根;否则在任意深度按 basename 匹配。
    bool anchored = false;
    if (body[0] == '/') {
        anchored = true;
        body.erase(body.begin());
        if (body.empty()) return;
    } else if (body.find('/') != std::string::npos) {
        anchored = true;
    }

    try {
        rule.regex = std::make_shared<std::regex>(
            "^" + translate_gitignore_pattern(body, anchored) + "$",
            std::regex::ECMAScript);
    } catch (const std::regex_error&) {
        // 非法 pattern(极端字符类等):丢弃该规则,不影响其余
        return;
    }
    rules_.push_back(std::move(rule));
}

void WorktreeIncludeMatcher::add_patterns(const std::vector<std::string>& patterns) {
    for (const auto& p : patterns) add_pattern(p);
}

std::optional<bool> WorktreeIncludeMatcher::match_single(const std::string& path,
                                                         bool is_dir) const {
    std::optional<bool> verdict;
    for (const auto& rule : rules_) {
        if (rule.dir_only && !is_dir) continue;
        if (rule.regex && std::regex_match(path, *rule.regex)) {
            verdict = !rule.negated;
        }
    }
    return verdict;
}

bool WorktreeIncludeMatcher::matches(const std::string& relative_path) const {
    if (rules_.empty()) return false;
    std::string path = normalize_rel_path(relative_path);
    bool is_dir = false;
    if (!path.empty() && path.back() == '/') {
        is_dir = true;
        path.pop_back();
    }
    if (path.empty()) return false;

    // 祖先目录命中 ⇒ 内部路径全部视为命中(gitignore:被排除目录里的文件
    // 无法被子级取反拯救,这里的 include 语义同构)。
    std::size_t pos = 0;
    while ((pos = path.find('/', pos)) != std::string::npos) {
        if (match_single(path.substr(0, pos), /*is_dir=*/true).value_or(false)) {
            return true;
        }
        ++pos;
    }
    return match_single(path, is_dir).value_or(false);
}

WorktreeIncludePlan plan_worktree_include_copy(
    const std::vector<std::string>& gitignored_entries,
    const std::vector<std::string>& patterns) {
    WorktreeIncludePlan plan;
    WorktreeIncludeMatcher matcher;
    matcher.add_patterns(patterns);
    if (matcher.empty()) return plan;

    std::vector<std::string> collapsed_dirs;
    for (const auto& raw : gitignored_entries) {
        std::string entry = normalize_rel_path(raw);
        if (entry.empty()) continue;
        if (entry.back() == '/') {
            collapsed_dirs.push_back(entry);
        } else if (matcher.matches(entry)) {
            plan.files_to_copy.push_back(entry);
        }
    }

    // collapsed 目录的展开判定(照搬 Claude Code copyWorktreeIncludeFiles):
    // 只有 pattern 明确指向该目录内部时才付出二次 ls-files 的成本。
    for (const auto& dir : collapsed_dirs) {
        bool expand = false;
        for (const auto& p : patterns) {
            std::string normalized = p;
            if (!normalized.empty() && normalized[0] == '/') normalized.erase(normalized.begin());
            // 字面前缀:pattern 以 collapsed 目录路径开头
            if (normalized.rfind(dir, 0) == 0) {
                expand = true;
                break;
            }
            // 锚定 glob:目录落在 pattern 的字面(非 glob)前缀之下
            // 例:"config/**/*.key" 的字面前缀 "config/" 覆盖 "config/secrets/"
            std::size_t glob_idx = normalized.find_first_of("*?[");
            if (glob_idx != std::string::npos && glob_idx > 0) {
                std::string literal_prefix = normalized.substr(0, glob_idx);
                if (dir.rfind(literal_prefix, 0) == 0) {
                    expand = true;
                    break;
                }
            }
        }
        if (!expand && matcher.matches(dir)) expand = true;
        if (expand) plan.dirs_to_expand.push_back(dir);
    }
    return plan;
}

} // namespace acecode::worktree
