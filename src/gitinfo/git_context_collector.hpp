#pragma once

#include <string>
#include <vector>

// git 感知的子进程采集层(openspec add-git-context)。所有函数同步阻塞,
// 调用方是 AgentLoop worker 线程或 daemon HTTP handler 线程,不在 UI 线程。
//
// 护栏(design D5):status/log 走 --no-optional-locks(只读不写 index.lock,
// 不与用户开着的 IDE 抢锁);全部命令 GIT_TERMINAL_PROMPT=0 + 超时,超时视为
// 失败静默降级 —— git 感知永远 best-effort,绝不阻塞会话。
namespace acecode::gitinfo {

inline constexpr int kDefaultGitTimeoutMs = 3000;

// 采集会话级 gitStatus 快照文本(format_git_status_snapshot 的产物)。
// 非 git 仓库、git 缺失、status/log 任一失败 → 返回空串(整块不注入)。
// user.name 失败仅省行,不算失败。
std::string collect_git_status_snapshot(const std::string& cwd,
                                        int timeout_ms = kDefaultGitTimeoutMs);

// /api/git/info 的数据面。
struct GitInfo {
    bool is_repo = false;
    std::string branch;          // detached / 失败 → "HEAD"
    std::string default_branch;  // 解析失败 → "main"(prompt 快照渲染需要一个名字)
    std::string default_base;    // 已验证存在的 "origin/<默认分支>";无 origin
                                 // remote / 从未 fetch → 空串。变更面板的基线
                                 // 候选只能用这个字段 —— default_branch 的
                                 // "main" 兜底是猜的,拼成 origin/main 喂给
                                 // /api/git/changes 会 invalid_base
    std::vector<std::string> branches; // 本地分支短名(for-each-ref 顺序)
    bool dirty = false;          // tracked 改动(status --porcelain -uno 非空)
};

// 采集仓库信息。cwd 非 git 仓库时返回 is_repo=false 且不 spawn git。
GitInfo collect_git_info(const std::string& cwd,
                         int timeout_ms = kDefaultGitTimeoutMs);

// 纯 fs 判定 cwd 是否在 git 仓库内(worktree::find_git_root 的转发,
// 让 prompt 层不用直接依赖 worktree 头文件)。
bool is_inside_git_repo(const std::string& cwd);

// ---- 写操作(openspec add-webui-git-session-pill,checkout 端点专用)----

struct GitOpResult {
    bool ok = false;
    std::string error; // git stderr(trim 后),给前端 toast
};

// tracked 改动的文件路径列表(status --porcelain -uno 的 path 列)。
// 空 = clean(或查询失败 —— 调用方用 has_error 区分)。
struct TrackedChanges {
    bool query_ok = false;
    std::vector<std::string> files;
};
TrackedChanges list_tracked_changes(const std::string& cwd,
                                    int timeout_ms = kDefaultGitTimeoutMs);

// stash 全部改动(含 untracked):`git stash push --include-untracked -m
// "ACECode auto-stash"`。-u 一条命令等价于 claude-code-haha 的「先 add
// untracked 再 stash」,且避开 Windows argv 长度上限。
GitOpResult stash_all_changes(const std::string& cwd,
                              int timeout_ms = kDefaultGitTimeoutMs);

// checkout 本地分支。branch 必须已过 is_safe_ref_name(调用方职责)。
// checkout 可能因冲突失败 —— git 的错误原样透传,不重试不 force。
GitOpResult checkout_branch(const std::string& cwd, const std::string& branch,
                            int timeout_ms = kDefaultGitTimeoutMs);

// ---- 变更列表 / 单文件 diff(openspec redesign-sidepanel-git-changes)----

// 列表上限:超出截断并置 truncated(不静默,前端显示「还有 N 个」)。
inline constexpr std::size_t kMaxChangeEntries = 200;
// 单文件 patch 上限(字节),超出交给前端显示「diff 过大」。
inline constexpr std::size_t kMaxFileDiffBytes = 1024 * 1024;

struct GitChangeEntry {
    std::string path;      // 相对仓库根;rename 显示新路径
    std::string status;    // "M" | "A" | "D" | "R" | "T"(untracked 归 "A")
    int additions = -1;    // -1 = 未知(二进制 / untracked)
    int deletions = -1;
    bool binary = false;
};

struct GitChangesList {
    bool ok = false;
    // 失败语义:"invalid_base"(base 不存在/不合法)| "timeout" | "git_error"
    std::string error_kind;
    std::string branch;          // 当前分支(detached → "HEAD")
    std::string base;            // 实际使用的比较基线
    std::vector<GitChangeEntry> files;
    int total_additions = 0;     // 全量(不受截断影响)
    int total_deletions = 0;
    std::size_t total_count = 0; // 全量文件数(含 untracked)
    bool truncated = false;
};

// 工作区(含未提交改动)相对 base 的差异 + untracked 文件。
// base 必须是 "HEAD" 或已过 is_safe_ref_name 的 ref 名;这里再做
// rev-parse --verify 存在性校验,不存在 → error_kind="invalid_base"。
GitChangesList list_git_changes(const std::string& cwd, const std::string& base,
                                int timeout_ms = kDefaultGitTimeoutMs);

struct GitFileDiff {
    bool ok = false;
    // "invalid_base" | "too_large" | "timeout" | "git_error"
    std::string error_kind;
    std::string patch;   // unified diff(可能为空 = 无差异)
};

// 单文件相对 base 的 patch。path 为相对仓库路径(调用方已做越权校验);
// untracked 文件走 --no-index 与空文件比对合成新增 patch。
GitFileDiff get_file_diff(const std::string& cwd, const std::string& base,
                          const std::string& path,
                          int timeout_ms = kDefaultGitTimeoutMs);

} // namespace acecode::gitinfo
