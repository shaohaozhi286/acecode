#include "git_context_collector.hpp"

#include "../worktree/worktree_manager.hpp"
#include "git_context_core.hpp"

#include <map>
#include <sstream>

namespace acecode::gitinfo {

namespace {

std::string trim(const std::string& s) {
    std::size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    std::size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// run_git 包一层:统一 no_prompt=true(凭据提示不可能有人在看)+ 调用方超时。
worktree::GitResult run(const std::vector<std::string>& args,
                        const std::string& cwd, int timeout_ms) {
    return worktree::run_git(args, cwd, timeout_ms, /*no_prompt=*/true);
}

// 默认分支解析,镜像 worktree::default_branch 但带显式超时(那边的 30s
// 默认超时在快照采集的关键路径上不可接受)。
std::string resolve_default_branch(const std::string& cwd, int timeout_ms) {
    auto symref = run({"symbolic-ref", "--short", "refs/remotes/origin/HEAD"},
                      cwd, timeout_ms);
    if (symref.ok()) {
        std::string ref = trim(symref.out); // "origin/main"
        const std::string prefix = "origin/";
        if (ref.rfind(prefix, 0) == 0 && ref.size() > prefix.size()) {
            std::string name = ref.substr(prefix.size());
            if (is_safe_ref_name(name)) return name;
        }
    }
    for (const char* candidate : {"main", "master"}) {
        auto check = run({"show-ref", "--verify", "--quiet",
                          std::string("refs/remotes/origin/") + candidate},
                         cwd, timeout_ms);
        if (check.ok()) return candidate;
    }
    return "main";
}

// 当前分支(rev-parse --abbrev-ref HEAD)。detached → "HEAD";失败/不安全 → ""。
std::string resolve_current_branch(const std::string& cwd, int timeout_ms) {
    auto res = run({"rev-parse", "--abbrev-ref", "HEAD"}, cwd, timeout_ms);
    if (!res.ok()) return "";
    std::string branch = trim(res.out);
    if (branch.empty() || branch == "HEAD") return branch;
    if (!is_safe_ref_name(branch)) return "";
    return branch;
}

} // namespace

bool is_inside_git_repo(const std::string& cwd) {
    return !worktree::find_git_root(cwd).empty();
}

std::string collect_git_status_snapshot(const std::string& cwd, int timeout_ms) {
    if (!is_inside_git_repo(cwd)) return "";

    // status / log 是快照的主体,任一失败整块放弃(半份快照会误导模型)。
    auto status = run({"--no-optional-locks", "status", "--short"}, cwd, timeout_ms);
    if (!status.ok()) return "";
    auto log = run({"--no-optional-locks", "log", "--oneline", "-n", "5"},
                   cwd, timeout_ms);
    if (!log.ok()) return "";

    SnapshotParts parts;
    parts.branch = resolve_current_branch(cwd, timeout_ms);
    if (parts.branch == "HEAD") parts.branch.clear(); // detached → 渲染层兜底
    parts.default_branch = resolve_default_branch(cwd, timeout_ms);
    parts.status_short = trim(status.out);
    parts.log_oneline = trim(log.out);

    // user.name 失败仅省行(新装机器常见未配置,不能因此丢整个快照)。
    auto user = run({"config", "user.name"}, cwd, timeout_ms);
    if (user.ok()) parts.user_name = trim(user.out);

    return format_git_status_snapshot(parts);
}

TrackedChanges list_tracked_changes(const std::string& cwd, int timeout_ms) {
    TrackedChanges result;
    auto porcelain = run({"--no-optional-locks", "status", "--porcelain", "-uno"},
                         cwd, timeout_ms);
    if (!porcelain.ok()) return result;
    result.query_ok = true;
    std::istringstream lines(porcelain.out);
    std::string line;
    while (std::getline(lines, line)) {
        // porcelain v1:两列状态 + 空格 + 路径("R  old -> new" 取整段展示)。
        if (line.size() > 3) {
            std::string path = trim(line.substr(3));
            if (!path.empty()) result.files.push_back(std::move(path));
        }
    }
    return result;
}

GitOpResult stash_all_changes(const std::string& cwd, int timeout_ms) {
    GitOpResult op;
    auto res = run({"stash", "push", "--include-untracked", "-m",
                    "ACECode auto-stash"},
                   cwd, timeout_ms);
    op.ok = res.ok();
    if (!op.ok) {
        op.error = trim(res.err);
        if (op.error.empty()) op.error = "git stash push failed";
    }
    return op;
}

GitOpResult checkout_branch(const std::string& cwd, const std::string& branch,
                            int timeout_ms) {
    GitOpResult op;
    if (!is_safe_ref_name(branch)) {
        op.error = "invalid branch name";
        return op;
    }
    auto res = run({"checkout", branch}, cwd, timeout_ms);
    op.ok = res.ok();
    if (!op.ok) {
        op.error = trim(res.err);
        if (op.error.empty()) op.error = "git checkout failed";
    }
    return op;
}

namespace {

// numstat 的 rename 路径形态归一:"prefix{old => new}suffix" 或
// "old => new" → 新路径。普通路径原样返回。
std::string normalize_numstat_path(const std::string& raw) {
    auto brace_open = raw.find('{');
    auto arrow = raw.find(" => ");
    if (arrow == std::string::npos) return raw;
    if (brace_open != std::string::npos) {
        auto brace_close = raw.find('}', brace_open);
        if (brace_close != std::string::npos && arrow > brace_open &&
            arrow < brace_close) {
            return raw.substr(0, brace_open) +
                   raw.substr(arrow + 4, brace_close - (arrow + 4)) +
                   raw.substr(brace_close + 1);
        }
    }
    return raw.substr(arrow + 4);
}

// base 校验 + 存在性验证。返回 false = invalid_base。
bool verify_base_ref(const std::string& cwd, const std::string& base,
                     int timeout_ms) {
    if (base != "HEAD" && !acecode::gitinfo::is_safe_ref_name(base)) return false;
    auto verify = run({"rev-parse", "--verify", "--quiet", base + "^{commit}"},
                      cwd, timeout_ms);
    return verify.ok();
}

} // namespace

GitChangesList list_git_changes(const std::string& cwd, const std::string& base,
                                int timeout_ms) {
    GitChangesList result;
    if (!is_inside_git_repo(cwd)) {
        result.error_kind = "git_error";
        return result;
    }
    if (!verify_base_ref(cwd, base, timeout_ms)) {
        result.error_kind = "invalid_base";
        return result;
    }
    result.base = base;
    result.branch = resolve_current_branch(cwd, timeout_ms);
    if (result.branch.empty()) result.branch = "HEAD";

    // numstat(行数)与 name-status(状态字母)各一条命令,按路径 join。
    // 都是只读索引/树比较,大仓库下也轻;diff 正文由 get_file_diff 按需拉。
    auto numstat = run({"--no-optional-locks", "diff", "--numstat", base},
                       cwd, timeout_ms);
    if (!numstat.ok()) {
        result.error_kind = numstat.exit_code == -1 ? "timeout" : "git_error";
        return result;
    }
    auto name_status = run({"--no-optional-locks", "diff", "--name-status", base},
                           cwd, timeout_ms);
    if (!name_status.ok()) {
        result.error_kind = name_status.exit_code == -1 ? "timeout" : "git_error";
        return result;
    }

    // path → 状态字母(R100 → R;rename/copy 记到新路径)。
    std::map<std::string, std::string> status_by_path;
    {
        std::istringstream lines(name_status.out);
        std::string line;
        while (std::getline(lines, line)) {
            if (line.empty()) continue;
            std::istringstream cols(line);
            std::string status, p1, p2;
            std::getline(cols, status, '\t');
            std::getline(cols, p1, '\t');
            std::getline(cols, p2, '\t');
            if (status.empty() || p1.empty()) continue;
            char kind = status[0];
            std::string path = (kind == 'R' || kind == 'C') && !p2.empty() ? p2 : p1;
            status_by_path[trim(path)] = std::string(1, kind);
        }
    }

    std::istringstream lines(numstat.out);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.empty()) continue;
        std::istringstream cols(line);
        std::string add_s, del_s, path_raw;
        std::getline(cols, add_s, '\t');
        std::getline(cols, del_s, '\t');
        std::getline(cols, path_raw);
        if (path_raw.empty()) continue;

        GitChangeEntry entry;
        entry.path = normalize_numstat_path(trim(path_raw));
        entry.binary = (add_s == "-" || del_s == "-");
        if (!entry.binary) {
            try {
                entry.additions = std::stoi(add_s);
                entry.deletions = std::stoi(del_s);
                result.total_additions += entry.additions;
                result.total_deletions += entry.deletions;
            } catch (const std::exception&) {
                entry.additions = -1;
                entry.deletions = -1;
            }
        }
        auto it = status_by_path.find(entry.path);
        entry.status = it != status_by_path.end() ? it->second : "M";

        ++result.total_count;
        if (result.files.size() < kMaxChangeEntries) {
            result.files.push_back(std::move(entry));
        }
    }

    // untracked 文件(diff 天然不含),status=A,行数未知。
    auto untracked = run({"ls-files", "--others", "--exclude-standard"},
                         cwd, timeout_ms);
    if (untracked.ok()) {
        std::istringstream ulines(untracked.out);
        std::string upath;
        while (std::getline(ulines, upath)) {
            upath = trim(upath);
            if (upath.empty()) continue;
            ++result.total_count;
            if (result.files.size() < kMaxChangeEntries) {
                GitChangeEntry entry;
                entry.path = upath;
                entry.status = "A";
                result.files.push_back(std::move(entry));
            }
        }
    }

    result.truncated = result.total_count > result.files.size();
    result.ok = true;
    return result;
}

GitFileDiff get_file_diff(const std::string& cwd, const std::string& base,
                          const std::string& path, int timeout_ms) {
    GitFileDiff result;
    if (!is_inside_git_repo(cwd)) {
        result.error_kind = "git_error";
        return result;
    }
    if (!verify_base_ref(cwd, base, timeout_ms)) {
        result.error_kind = "invalid_base";
        return result;
    }

    auto diff = run({"--no-optional-locks", "diff", base, "--", path},
                    cwd, timeout_ms);
    if (!diff.ok()) {
        result.error_kind = diff.exit_code == -1 ? "timeout" : "git_error";
        return result;
    }

    std::string patch = diff.out;
    if (patch.empty()) {
        // 相对 base 无差异且是 untracked 文件时,用 --no-index 合成"新增
        // 文件" patch。--no-index 有差异时退出码为 1 —— 是正常结果不是错误。
        auto no_index = worktree::run_git(
            {"--no-optional-locks", "diff", "--no-index", "--", "/dev/null", path},
            cwd, timeout_ms, /*no_prompt=*/true);
        if (no_index.started && !no_index.out.empty()) {
            patch = no_index.out;
        }
    }

    if (patch.size() > kMaxFileDiffBytes) {
        result.error_kind = "too_large";
        return result;
    }
    result.ok = true;
    result.patch = std::move(patch);
    return result;
}

GitInfo collect_git_info(const std::string& cwd, int timeout_ms) {
    GitInfo info;
    if (!is_inside_git_repo(cwd)) return info;
    info.is_repo = true;

    std::string branch = resolve_current_branch(cwd, timeout_ms);
    info.branch = branch.empty() ? std::string("HEAD") : branch;
    info.default_branch = resolve_default_branch(cwd, timeout_ms);

    auto refs = run({"for-each-ref", "--format=%(refname:short)", "refs/heads"},
                    cwd, timeout_ms);
    if (refs.ok()) {
        std::istringstream lines(refs.out);
        std::string line;
        while (std::getline(lines, line)) {
            std::string name = trim(line);
            if (!name.empty() && is_safe_ref_name(name)) {
                info.branches.push_back(std::move(name));
            }
        }
    }

    // -uno:只看 tracked 改动。dirty 的消费方是 checkout 安全门(后续
    // change),untracked 文件不会被 checkout 拦住,不算 dirty。
    auto porcelain = run({"--no-optional-locks", "status", "--porcelain", "-uno"},
                         cwd, timeout_ms);
    info.dirty = porcelain.ok() && !trim(porcelain.out).empty();

    return info;
}

} // namespace acecode::gitinfo
