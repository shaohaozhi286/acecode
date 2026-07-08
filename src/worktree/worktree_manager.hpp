#pragma once

#include "worktree_core.hpp"

#include <optional>
#include <string>
#include <vector>

// Worktree 的 git 子进程操作层,复刻 Claude Code src/utils/worktree.ts 中
// 依赖 execFileNoThrow 的部分。所有函数都是同步阻塞的 —— 调用方是工具执行
// 线程(AgentLoop worker)或启动流程,不在 UI 线程上。
namespace acecode::worktree {

struct GitResult {
    bool started = false;
    int exit_code = -1;
    std::string out;   // stdout(UTF-8)
    std::string err;   // stderr(UTF-8)
    bool ok() const { return started && exit_code == 0; }
};

// 运行 git 子进程并捕获输出。no_prompt=true 时在调用期间设置
// GIT_TERMINAL_PROMPT=0 / GIT_ASKPASS=""(进程级、调用后恢复),防止
// fetch 在等凭据时挂死 —— stdin 由 runner 直接关闭,交互式提示无法阻塞。
GitResult run_git(const std::vector<std::string>& args,
                  const std::string& cwd,
                  int timeout_ms = 30000,
                  bool no_prompt = false);

// 向上查找包含 .git(目录或 worktree 指针文件)的目录。"" = 不在 git 仓库。
std::string find_git_root(const std::string& start_dir);

// 穿透 linked worktree 找主仓根:worktree 内返回主仓根目录,普通仓库等同
// find_git_root。基于 `git rev-parse --git-common-dir`(信任 git 自身的
// 解析,不手工解析 .git 指针文件)。"" = 不在 git 仓库。
std::string find_canonical_git_root(const std::string& start_dir);

// 当前分支名(rev-parse --abbrev-ref HEAD)。detached HEAD 返回 "HEAD",
// 失败返回 ""。
std::string current_branch(const std::string& cwd);

// 默认分支:origin/HEAD symref → origin/main / origin/master 存在性 → "main"。
std::string default_branch(const std::string& repo_root);

struct WorktreeCreateOptions {
    std::optional<int> pr_number;           // 有值 = 基于 origin PR head 创建
    std::vector<std::string> sparse_paths;  // 非空 = sparse-checkout cone 模式
    // 非空 = 基于指定的本地分支创建(webui git session pill 的用户选择),
    // 分支必须存在(rev-parse --verify refs/heads/<name>),否则创建失败。
    // 与 pr_number 互斥,pr_number 优先。空 = 默认 origin/<默认分支> 策略。
    std::string base_branch;
};

struct WorktreeCreateResult {
    bool ok = false;
    std::string error;
    std::string worktree_path;
    std::string worktree_branch;
    std::string head_commit;   // 创建基线 SHA(existed 时为当前 HEAD)
    std::string base_ref;      // origin/<branch> / FETCH_HEAD / HEAD
    bool existed = false;      // true = 命中已有 worktree(fast resume)
};

// 创建或复用 slug 对应的 worktree。同名 worktree 跨调用复用同一路径,
// 存在性检查避免每次 resume 都跑可能挂在凭据上的 `git fetch`。
// 基线选择:PR → fetch pull/N/head;否则 origin/<默认分支> 本地已有就直接
// 用(大仓库 fetch 很贵,略旧的基线可接受),没有再 fetch,fetch 失败回退
// 当前 HEAD。分支用 -B 建(顺带复位删目录后残留的孤儿分支)。
WorktreeCreateResult get_or_create_worktree(const std::string& repo_root,
                                            const std::string& slug,
                                            const WorktreeCreateOptions& options = {});

struct PostCreationOptions {
    // 从主仓 symlink 进 worktree 的目录(如 node_modules),避免磁盘膨胀。
    // 显式配置才生效,默认不做。
    std::vector<std::string> symlink_directories;
};

// 新建 worktree 的后处理:core.hooksPath 指回主仓(.husky 优先)、
// 配置目录 symlink、按 .worktreeinclude 拷贝 gitignored 文件。
// 全部 best-effort,失败只记日志不抛。
void perform_post_creation_setup(const std::string& repo_root,
                                 const std::string& worktree_path,
                                 const PostCreationOptions& options = {});

// 把主仓中「被 gitignore 且命中 .worktreeinclude 模式」的文件拷进 worktree。
// 用 --directory 让完全被忽略的目录折叠成单条目(node_modules 不用全量遍历),
// pattern 明确指向折叠目录内部时才做二次展开。返回实际拷贝的相对路径列表。
std::vector<std::string> copy_worktree_include_files(const std::string& repo_root,
                                                     const std::string& worktree_path);

struct ChangeSummary {
    int changed_files = 0;  // git status --porcelain 非空行数
    int commits = 0;        // original_head_commit..HEAD 的提交数
};

// 统计 worktree 相对创建基线的变更。fail-closed:git 失败或没有基线 SHA
// 时返回 nullopt —— 调用方把 nullopt 当"未知,视为有变更"处理,静默的
// 0/0 会让删除路径毁掉真实工作。
std::optional<ChangeSummary> count_worktree_changes(const std::string& worktree_path,
                                                    const std::string& original_head_commit);

// 删除 worktree(git worktree remove --force,从主仓根执行 —— worktree
// 目录本身马上就没了)并删除临时分支。分支删除失败只记日志。
bool remove_worktree(const std::string& repo_root,
                     const std::string& worktree_path,
                     const std::string& worktree_branch,
                     std::string* error = nullptr);

// `git worktree list --porcelain` 的路径列表。失败返回空。
std::vector<std::string> list_worktree_paths(const std::string& cwd);

} // namespace acecode::worktree
