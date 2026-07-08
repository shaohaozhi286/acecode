#pragma once

#include <cstddef>
#include <string>

// git 感知的纯逻辑层(openspec add-git-context),复刻 Claude Code 的
// gitStatus 快照文本与安全校验。零 git 子进程 / 零文件系统依赖,进
// acecode_testable,单测不需要 git 环境。
//
// 仓库检测(向上找 .git)与分支解析复用 worktree 层的既有实现
// (worktree::find_git_root / current_branch / default_branch),这里
// 只承担"可以在无 git 环境下单测"的文本与校验逻辑。
namespace acecode::gitinfo {

// 快照 status 段的截断上限(字符数,与 Claude Code MAX_STATUS_CHARS 一致)。
inline constexpr std::size_t kMaxStatusChars = 2000;

// 校验从 .git 裸文件 / 外部输入读到的 ref / 分支名是否安全:
// 只允许 [A-Za-z0-9/._+@-],拒绝前导 '-' 或 '/'、'..'、空段与 '.' 段。
// .git/HEAD 是可被篡改的明文,内容会进 prompt、REST 响应,后续 change
// 还会拼进 git argv —— 必须白名单收口(对齐 claude-code-haha isSafeRefName)。
bool is_safe_ref_name(const std::string& name);

// 40(SHA-1)或 64(SHA-256)位小写 hex。git 不会往 HEAD/ref 文件写缩写 SHA。
bool is_valid_git_sha(const std::string& s);

struct SnapshotParts {
    std::string branch;         // 空 = 未知/detached,渲染为 "HEAD"
    std::string default_branch; // 空 = 未知,渲染为 "main"
    std::string user_name;      // 空 = 省略该行
    std::string status_short;   // `git status --short` 原文(可空 = clean)
    std::string log_oneline;    // `git log --oneline -n 5` 原文
};

// 拼装 gitStatus 快照文本(英文,给模型消费),结构与 Claude Code 相同:
// snapshot-in-time 声明 / Current branch / Main branch / Git user /
// Status(空 → "(clean)",超 kMaxStatusChars 截断并附提示)/ Recent commits。
std::string format_git_status_snapshot(const SnapshotParts& parts);

// status 段截断(UTF-8 安全,不切多字节序列):超限时截到上限并追加
// 换行 + 截断说明。导出供单测直接覆盖边界。
std::string truncate_status_for_snapshot(const std::string& status,
                                         std::size_t max_chars = kMaxStatusChars);

} // namespace acecode::gitinfo
