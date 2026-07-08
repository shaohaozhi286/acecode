#include "worktree_manager.hpp"

#include "../hooks/hook_config.hpp"
#include "../hooks/hook_runner.hpp"
#include "../utils/logger.hpp"
#include "../utils/utf8_path.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace acecode::worktree {

namespace {

std::string trim(const std::string& s) {
    std::size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    std::size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// 调用期间临时覆盖环境变量并在析构时恢复。进程级副作用 —— 与并发
// 创建的其它子进程(PTY / bash)存在极小的窗口重叠,影响仅限"该子进程
// 内 git 不弹凭据提示",可接受;换取的是 fetch 永不挂死在凭据输入上。
class ScopedEnvVar {
public:
    ScopedEnvVar(const char* name, const char* value) : name_(name) {
        const char* old = std::getenv(name);
        if (old) {
            had_old_ = true;
            old_value_ = old;
        }
        set(name, value);
    }
    ~ScopedEnvVar() {
        if (had_old_) {
            set(name_.c_str(), old_value_.c_str());
        } else {
            unset(name_.c_str());
        }
    }

private:
    static void set(const char* name, const char* value) {
#ifdef _WIN32
        _putenv_s(name, value);
#else
        setenv(name, value, 1);
#endif
    }
    static void unset(const char* name) {
#ifdef _WIN32
        _putenv_s(name, "");
#else
        unsetenv(name);
#endif
    }

    std::string name_;
    std::string old_value_;
    bool had_old_ = false;
};

// no_prompt 的环境覆盖需要互斥:两个并发的 fetch 交错 set/restore 会把
// 用户原值弄丢。worktree 操作本身低频,串行化没有可见代价。
std::mutex& git_env_mu() {
    static std::mutex mu;
    return mu;
}

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) out.push_back(line);
    }
    return out;
}

std::string canonical_or_original(const std::string& p) {
    std::error_code ec;
    fs::path canon = fs::weakly_canonical(path_from_utf8(p), ec);
    if (ec || canon.empty()) return p;
    return path_to_utf8(canon);
}

} // namespace

GitResult run_git(const std::vector<std::string>& args,
                  const std::string& cwd,
                  int timeout_ms,
                  bool no_prompt) {
    HookCommandSpec spec;
    spec.command = "git";
    spec.args = args;

    HookProcessResult raw;
    if (no_prompt) {
        std::lock_guard<std::mutex> lk(git_env_mu());
        ScopedEnvVar terminal_prompt("GIT_TERMINAL_PROMPT", "0");
        ScopedEnvVar askpass("GIT_ASKPASS", "");
        raw = run_hook_process(spec, "", timeout_ms, cwd);
    } else {
        raw = run_hook_process(spec, "", timeout_ms, cwd);
    }

    GitResult result;
    result.started = raw.started;
    result.exit_code = raw.timed_out ? -1 : raw.exit_code;
    result.out = raw.stdout_text;
    result.err = raw.error.empty() ? raw.stderr_text
                                   : raw.stderr_text + raw.error;
    return result;
}

std::string find_git_root(const std::string& start_dir) {
    std::error_code ec;
    fs::path current = fs::absolute(path_from_utf8(start_dir), ec);
    if (ec || current.empty()) return "";
    current = current.lexically_normal();
    while (true) {
        // .git 可以是目录(普通仓库)或文件(worktree / submodule 指针)
        if (fs::exists(current / ".git", ec)) {
            return path_to_utf8(current);
        }
        fs::path parent = current.parent_path();
        if (parent.empty() || parent == current) return "";
        current = parent;
    }
}

std::string find_canonical_git_root(const std::string& start_dir) {
    std::string root = find_git_root(start_dir);
    if (root.empty()) return "";

    auto res = run_git({"rev-parse", "--git-common-dir"}, root);
    if (!res.ok()) return root;
    std::string common = trim(res.out);
    if (common.empty()) return root;

    fs::path common_path = path_from_utf8(common);
    if (common_path.is_relative()) {
        common_path = path_from_utf8(root) / common_path;
    }
    std::error_code ec;
    fs::path canon = fs::weakly_canonical(common_path, ec);
    if (!ec && !canon.empty()) common_path = canon;

    // 普通仓库与 worktree 的 common dir 都是 <主仓根>/.git;
    // 形状不符(bare 仓库等)时保守回退当前 root。
    if (common_path.filename() != ".git") return root;
    return path_to_utf8(common_path.parent_path());
}

std::string current_branch(const std::string& cwd) {
    auto res = run_git({"rev-parse", "--abbrev-ref", "HEAD"}, cwd);
    if (!res.ok()) return "";
    return trim(res.out);
}

std::string default_branch(const std::string& repo_root) {
    auto symref = run_git({"symbolic-ref", "--short", "refs/remotes/origin/HEAD"},
                          repo_root);
    if (symref.ok()) {
        std::string ref = trim(symref.out); // "origin/main"
        const std::string prefix = "origin/";
        if (ref.rfind(prefix, 0) == 0 && ref.size() > prefix.size()) {
            return ref.substr(prefix.size());
        }
    }
    for (const char* candidate : {"main", "master"}) {
        auto check = run_git({"show-ref", "--verify", "--quiet",
                              std::string("refs/remotes/origin/") + candidate},
                             repo_root);
        if (check.ok()) return candidate;
    }
    return "main";
}

WorktreeCreateResult get_or_create_worktree(const std::string& repo_root,
                                            const std::string& slug,
                                            const WorktreeCreateOptions& options) {
    WorktreeCreateResult result;
    result.worktree_path = worktree_path_for(repo_root, slug);
    result.worktree_branch = worktree_branch_name(slug);

    // Fast resume:worktree 已存在(HEAD 可解析)时跳过 fetch 与创建。
    std::error_code ec;
    if (fs::exists(path_from_utf8(result.worktree_path) / ".git", ec)) {
        auto head = run_git({"rev-parse", "HEAD"}, result.worktree_path);
        if (head.ok()) {
            result.ok = true;
            result.existed = true;
            result.head_commit = trim(head.out);
            return result;
        }
    }

    fs::create_directories(path_from_utf8(repo_root) / ".acecode" / "worktrees", ec);

    // 目录被手工删掉(非 git worktree remove)时,git 里还挂着注册项,
    // 直接 add 会报 "missing but already registered"。prune 一次(~15ms)
    // 消掉这类残骸;正常路径下是 no-op。
    run_git({"worktree", "prune"}, repo_root);

    std::string base_ref;
    std::string base_sha;
    if (options.pr_number.has_value()) {
        auto pr_fetch = run_git(
            {"fetch", "origin", "pull/" + std::to_string(*options.pr_number) + "/head"},
            repo_root, /*timeout_ms=*/120000, /*no_prompt=*/true);
        if (!pr_fetch.ok()) {
            std::string detail = trim(pr_fetch.err);
            result.error = "Failed to fetch PR #" + std::to_string(*options.pr_number) +
                           ": " + (detail.empty()
                                       ? "PR may not exist or the repository may not have "
                                         "a remote named \"origin\""
                                       : detail);
            return result;
        }
        base_ref = "FETCH_HEAD";
    } else if (!options.base_branch.empty()) {
        // 用户显式选择的本地基线分支(webui git session pill):必须真实
        // 存在,否则报错而不是静默回退 —— 用户点名的基线被换掉是惊吓。
        auto verify = run_git({"rev-parse", "--verify", "--quiet",
                               "refs/heads/" + options.base_branch},
                              repo_root);
        if (!verify.ok()) {
            result.error = "Base branch \"" + options.base_branch +
                           "\" does not exist in this repository";
            return result;
        }
        base_ref = options.base_branch;
        base_sha = trim(verify.out);
    } else {
        // origin/<默认分支> 本地已有就不 fetch:大仓库 fetch 动辄数秒,
        // 略旧的基线可以接受(用户可以在 worktree 里自己 pull)。
        const std::string def = default_branch(repo_root);
        const std::string origin_ref = "origin/" + def;
        auto local = run_git({"rev-parse", "--verify", "--quiet",
                              "refs/remotes/" + origin_ref},
                             repo_root);
        if (local.ok()) {
            base_ref = origin_ref;
            base_sha = trim(local.out);
        } else {
            auto fetch = run_git({"fetch", "origin", def}, repo_root,
                                 /*timeout_ms=*/120000, /*no_prompt=*/true);
            base_ref = fetch.ok() ? origin_ref : "HEAD";
        }
    }

    if (base_sha.empty()) {
        auto sha = run_git({"rev-parse", base_ref}, repo_root);
        if (!sha.ok()) {
            result.error = "Failed to resolve base branch \"" + base_ref +
                           "\": git rev-parse failed";
            return result;
        }
        base_sha = trim(sha.out);
    }

    std::vector<std::string> add_args = {"worktree", "add"};
    if (!options.sparse_paths.empty()) add_args.push_back("--no-checkout");
    // -B 而不是 -b:顺带复位"删了目录但分支还在"的孤儿分支,免去先
    // branch -D 一趟。
    add_args.insert(add_args.end(),
                    {"-B", result.worktree_branch, result.worktree_path, base_ref});
    auto create = run_git(add_args, repo_root, /*timeout_ms=*/120000);
    if (!create.ok()) {
        result.error = "Failed to create worktree: " + trim(create.err);
        return result;
    }

    if (!options.sparse_paths.empty()) {
        // --no-checkout 之后 sparse-checkout / checkout 失败的话,worktree
        // 已注册、HEAD 已设,但工作区是空的 —— 下次 fast-resume 会把坏
        // worktree 当"已恢复"。先拆掉再报错。
        auto tear_down = [&](const std::string& msg) {
            run_git({"worktree", "remove", "--force", result.worktree_path}, repo_root);
            result.error = msg;
        };
        std::vector<std::string> sparse_args = {"sparse-checkout", "set", "--cone", "--"};
        sparse_args.insert(sparse_args.end(), options.sparse_paths.begin(),
                           options.sparse_paths.end());
        auto sparse = run_git(sparse_args, result.worktree_path);
        if (!sparse.ok()) {
            tear_down("Failed to configure sparse-checkout: " + trim(sparse.err));
            return result;
        }
        auto co = run_git({"checkout", "HEAD"}, result.worktree_path,
                          /*timeout_ms=*/120000);
        if (!co.ok()) {
            tear_down("Failed to checkout sparse worktree: " + trim(co.err));
            return result;
        }
    }

    result.ok = true;
    result.head_commit = base_sha;
    result.base_ref = base_ref;
    return result;
}

std::vector<std::string> copy_worktree_include_files(const std::string& repo_root,
                                                     const std::string& worktree_path) {
    std::vector<std::string> copied;

    std::string include_content;
    {
        std::ifstream in(path_from_utf8(repo_root) / ".worktreeinclude",
                         std::ios::binary);
        if (!in) return copied;
        std::ostringstream buf;
        buf << in.rdbuf();
        include_content = buf.str();
    }
    const auto patterns = parse_worktree_include_patterns(include_content);
    if (patterns.empty()) return copied;

    // --directory 把完全被忽略的目录折叠成单条目(node_modules/ 一条而不是
    // 几十万条),大仓库从数秒降到百毫秒级。
    auto gitignored = run_git({"ls-files", "--others", "--ignored",
                               "--exclude-standard", "--directory"},
                              repo_root, /*timeout_ms=*/60000);
    if (!gitignored.ok()) return copied;

    auto plan = plan_worktree_include_copy(split_lines(gitignored.out), patterns);

    if (!plan.dirs_to_expand.empty()) {
        std::vector<std::string> expand_args = {"ls-files", "--others", "--ignored",
                                                "--exclude-standard", "--"};
        expand_args.insert(expand_args.end(), plan.dirs_to_expand.begin(),
                           plan.dirs_to_expand.end());
        auto expanded = run_git(expand_args, repo_root, /*timeout_ms=*/60000);
        if (expanded.ok()) {
            WorktreeIncludeMatcher matcher;
            matcher.add_patterns(patterns);
            for (const auto& f : split_lines(expanded.out)) {
                if (matcher.matches(f)) plan.files_to_copy.push_back(f);
            }
        }
    }

    for (const auto& relative : plan.files_to_copy) {
        fs::path src = path_from_utf8(repo_root) / path_from_utf8(relative);
        fs::path dst = path_from_utf8(worktree_path) / path_from_utf8(relative);
        std::error_code ec;
        fs::create_directories(dst.parent_path(), ec);
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            LOG_WARN("[worktree] failed to copy " + relative + " to worktree: " +
                     ec.message());
        } else {
            copied.push_back(relative);
        }
    }
    return copied;
}

void perform_post_creation_setup(const std::string& repo_root,
                                 const std::string& worktree_path,
                                 const PostCreationOptions& options) {
    // 让 worktree 用主仓的 git hooks(.husky 等用相对路径的 hook 在
    // worktree 里会解析到不存在的位置)。git config 不带 --worktree 写的
    // 是主仓 .git/config,所有 worktree 共享 —— 值已经对时跳过子进程。
    std::error_code ec;
    std::string hooks_path;
    for (const fs::path candidate : {path_from_utf8(repo_root) / ".husky",
                                     path_from_utf8(repo_root) / ".git" / "hooks"}) {
        if (fs::is_directory(candidate, ec)) {
            hooks_path = path_to_utf8(candidate);
            break;
        }
    }
    if (!hooks_path.empty()) {
        auto existing = run_git({"config", "--get", "core.hooksPath"}, worktree_path);
        if (!existing.ok() || trim(existing.out) != hooks_path) {
            auto set = run_git({"config", "core.hooksPath", hooks_path}, worktree_path);
            if (!set.ok()) {
                LOG_WARN("[worktree] failed to configure hooks path: " + trim(set.err));
            }
        }
    }

    for (const auto& dir : options.symlink_directories) {
        // 拒绝会逃出仓库边界的配置(绝对路径 / ".." 段)
        fs::path rel = path_from_utf8(dir);
        bool traversal = rel.is_absolute();
        for (const auto& part : rel) {
            if (part == "..") traversal = true;
        }
        if (traversal || dir.empty()) {
            LOG_WARN("[worktree] skipping symlink for \"" + dir +
                     "\": path traversal detected");
            continue;
        }
        fs::path src = path_from_utf8(repo_root) / rel;
        fs::path dst = path_from_utf8(worktree_path) / rel;
        std::error_code link_ec;
        if (!fs::exists(src, link_ec)) continue;  // 源还不存在:静默跳过
        if (fs::exists(dst, link_ec)) continue;   // 目标已存在:静默跳过
        fs::create_directory_symlink(src, dst, link_ec);
        if (link_ec) {
            // Windows 下无开发者模式/管理员权限时 CreateSymbolicLink 会拒绝
            LOG_WARN("[worktree] failed to symlink " + dir + ": " + link_ec.message());
        }
    }

    copy_worktree_include_files(repo_root, worktree_path);
}

std::optional<ChangeSummary> count_worktree_changes(
    const std::string& worktree_path,
    const std::string& original_head_commit) {
    auto status = run_git({"status", "--porcelain"}, worktree_path,
                          /*timeout_ms=*/60000);
    if (!status.ok()) return std::nullopt;

    ChangeSummary summary;
    summary.changed_files = static_cast<int>(split_lines(status.out).size());

    if (original_head_commit.empty()) {
        // status 成功说明确实是 git 工作区,但没有基线就数不了提交,
        // fail-closed 而不是谎报 0。
        return std::nullopt;
    }

    auto rev_list = run_git({"rev-list", "--count", original_head_commit + "..HEAD"},
                            worktree_path, /*timeout_ms=*/60000);
    if (!rev_list.ok()) return std::nullopt;
    try {
        summary.commits = std::stoi(trim(rev_list.out));
    } catch (...) {
        return std::nullopt;
    }
    return summary;
}

bool remove_worktree(const std::string& repo_root,
                     const std::string& worktree_path,
                     const std::string& worktree_branch,
                     std::string* error) {
    // 从主仓根执行 —— worktree 目录本身马上会被删掉,不能拿它当 cwd。
    auto remove = run_git({"worktree", "remove", "--force", worktree_path},
                          repo_root, /*timeout_ms=*/120000);
    if (!remove.ok()) {
        if (error) *error = trim(remove.err);
        LOG_WARN("[worktree] failed to remove worktree: " + trim(remove.err));
        return false;
    }

    if (!worktree_branch.empty()) {
        auto del = run_git({"branch", "-D", worktree_branch}, repo_root);
        if (!del.ok()) {
            // 分支删除失败不算致命(worktree 已经没了),记日志即可
            LOG_WARN("[worktree] could not delete worktree branch: " + trim(del.err));
        }
    }
    return true;
}

std::vector<std::string> list_worktree_paths(const std::string& cwd) {
    std::vector<std::string> out;
    auto res = run_git({"worktree", "list", "--porcelain"}, cwd);
    if (!res.ok()) return out;
    const std::string prefix = "worktree ";
    for (const auto& line : split_lines(res.out)) {
        if (line.rfind(prefix, 0) == 0) {
            out.push_back(canonical_or_original(line.substr(prefix.size())));
        }
    }
    return out;
}

} // namespace acecode::worktree
