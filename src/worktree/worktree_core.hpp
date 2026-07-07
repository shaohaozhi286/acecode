#pragma once

#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <vector>

// Worktree 纯逻辑层(无 git 子进程 / 无文件系统副作用),复刻 Claude Code
// src/utils/worktree.ts 的对应纯函数。子进程相关操作在 worktree_manager。
// 进 acecode_testable,单测不需要 git 环境。
namespace acecode::worktree {

// worktree 目录固定放在主仓根的 .acecode/worktrees/ 下(对齐 Claude Code 的
// .claude/worktrees/,换成本项目的数据目录名)。
inline constexpr const char* kWorktreesRelativeDir = ".acecode/worktrees";

// 分支名前缀。slug "foo" → 分支 "worktree-foo"。
inline constexpr const char* kWorktreeBranchPrefix = "worktree-";

inline constexpr std::size_t kMaxWorktreeSlugLength = 64;

// 校验 worktree slug,防路径穿越/目录逃逸。返回空字符串 = 合法,
// 否则返回人类可读的错误原因。
// 规则(与 Claude Code validateWorktreeSlug 一致):
//   - 总长 ≤ 64
//   - 按 "/" 分段,每段只允许 [A-Za-z0-9._-] 且非空
//   - 段不能是 "." 或 ".."(防止 join 后逃出 worktrees 目录)
std::string validate_worktree_slug(const std::string& slug);

// 嵌套 slug 扁平化:"user/feature" → "user+feature"。
// 分支与目录都用扁平化结果 —— 嵌套在两边都不安全:
//   - git ref:worktree-user(文件)与 worktree-user/feature(需要目录)
//     是 D/F 冲突,git 直接拒绝;
//   - 目录:.acecode/worktrees/user/feature/ 落在 user worktree 里面,
//     对父级 `git worktree remove` 会连带删掉子级未提交的工作。
// "+" 在 git 分支名和文件路径里合法,但不在 slug 段白名单里,映射是单射。
std::string flatten_slug(const std::string& slug);

// slug → 分支名:"worktree-" + flatten_slug(slug)。
std::string worktree_branch_name(const std::string& slug);

// slug → worktree 绝对路径:<repo_root>/.acecode/worktrees/<flatten(slug)>。
std::string worktree_path_for(const std::string& repo_root, const std::string& slug);

// 解析 PR 引用:GitHub 风格 PR URL(https://<host>/owner/repo/pull/123,
// 兼容 GHE 域名、可带尾斜杠/query/hash)或 "#123" 格式。
// 不认识的输入返回 nullopt。/pull/N 的路径形状是 GitHub 特有的
// (GitLab 是 /-/merge_requests/N,Bitbucket 是 /pull-requests/N),
// 所以对任意 host 匹配是安全的。
std::optional<int> parse_pr_reference(const std::string& input);

// 生成随机 slug:"<形容词>-<名词>-<4位base36>"(与 Claude Code 的
// 随机命名词表一致)。seed 显式传入以便单测可复现;运行时调用方用
// std::random_device 取种子。
std::string generate_worktree_slug(unsigned seed);

// ---- .worktreeinclude 模式匹配 ------------------------------------------
// .worktreeinclude 使用 .gitignore 语法声明"哪些被 gitignore 的文件要拷进
// 新 worktree"(典型:.env、本地证书)。这里实现 gitignore 语义的一个够用
// 子集:注释/空行、`!` 取反(后者优先)、`*`/`?`/`**`/字符类、首 `/` 锚定、
// 中缀 `/` 锚定、尾 `/` 目录模式。未实现的角落语义(如尾随空格转义)按
// 字面处理。

// 解析 .worktreeinclude 文本 → 有效 pattern 列表(去空行/注释/首尾空白)。
std::vector<std::string> parse_worktree_include_patterns(const std::string& content);

// gitignore 风格匹配器。add 顺序即优先级(最后命中的 pattern 生效,
// 与 gitignore 相同);matches(path) 的 path 是相对仓库根、"/" 分隔、
// 不带前导 "/" 的文件或目录路径(目录可带尾 "/")。
// 与 npm ignore 库对齐的关键行为:pattern 命中路径的任一祖先目录时,
// 路径本身视为命中(目录被包含 ⇒ 里面的文件全部包含)。
class WorktreeIncludeMatcher {
public:
    void add_pattern(const std::string& pattern);
    void add_patterns(const std::vector<std::string>& patterns);

    // path 或其任一祖先目录被(非取反的最终结果)命中 → true。
    bool matches(const std::string& relative_path) const;

    bool empty() const { return rules_.empty(); }

private:
    struct Rule {
        std::shared_ptr<std::regex> regex; // 预编译;非法 pattern 在 add 时丢弃
        bool negated = false;              // "!" 前缀
        bool dir_only = false;             // 尾 "/":只命中目录(及其内部)
    };

    // 单条路径(不含祖先展开)对全部规则做 last-match-wins。
    // 返回 nullopt = 没有任何规则命中。
    std::optional<bool> match_single(const std::string& path, bool is_dir) const;

    std::vector<Rule> rules_;
};

// 从 `git ls-files --others --ignored --exclude-standard --directory` 的输出
// (collapsed 目录带尾 "/")与 .worktreeinclude patterns 计算:
//   files_to_copy    — 直接命中的普通文件
//   dirs_to_expand   — 需要二次 ls-files 展开的 collapsed 目录
// 展开判定与 Claude Code 相同:pattern 的字面前缀落在该目录下 / 锚定 glob
// 的字面前缀覆盖该目录 / 目录本身命中 matcher。`**/` 与无锚定 pattern 不
// 触发展开(否则每个 collapsed 目录都要展开,失去 --directory 的性能意义)。
struct WorktreeIncludePlan {
    std::vector<std::string> files_to_copy;
    std::vector<std::string> dirs_to_expand;
};
WorktreeIncludePlan plan_worktree_include_copy(
    const std::vector<std::string>& gitignored_entries,
    const std::vector<std::string>& patterns);

} // namespace acecode::worktree
