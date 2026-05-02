#pragma once

// desktop 端的 workspace 注册表。每个"workspace"对应一个 cwd 目录,在磁盘上
// 落地为 .acecode/projects/<cwd_hash>/workspace.json。
//
// 文件格式:
//   { "cwd": "<absolute-path>", "name": "<display-name>" }
//
// name 可由用户行内重命名;缺失或损坏时回退到 fs::path(cwd).filename()(再不济
// 用 root_name 与字面常量"workspace")。
//
// scan() 优先读取 workspace.json;缺失时会尝试从同目录下的 session meta
// 反推 cwd 并回填 workspace.json。完全无 cwd 线索的孤儿目录仍不入册。
//
// 所有操作线程安全,内部用 std::mutex 保护 entries_。所有写盘走 atomic_write
// (.tmp + rename),失败时内存 cache 回滚。

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace acecode::desktop {

struct WorkspaceMeta {
    std::string hash; // 16 位 hex,= compute_cwd_hash(cwd)
    std::string cwd;  // 绝对路径,正斜杠规范化前的原始字符串
    std::string name; // 显示名;用户未指定时 = default_workspace_name(cwd)
};

class WorkspaceRegistry {
public:
    WorkspaceRegistry() = default;

    // 扫 projects_dir 下所有子目录,把可读 + 含 cwd 字段的 workspace.json 入册。
    // 重复调用会清空再重扫(MVP 不做增量)。
    void scan(const std::string& projects_dir);

    // 当前快照。线程安全(内部加锁后 copy)。
    std::vector<WorkspaceMeta> list() const;

    // 查指定 hash;不存在返回 nullopt。
    std::optional<WorkspaceMeta> get(const std::string& hash) const;

    // 给一个 cwd 注册 workspace:
    //   - 已存在(hash 已知)→ 返回已有 meta,不动磁盘
    //   - 新 → 建目录 + 写 workspace.json(name = default),入册,返回新 meta
    // projects_dir 不会被扫(scan 是一次性);此处用调用方传入的根目录定位写入位置。
    WorkspaceMeta register_new(const std::string& projects_dir, const std::string& cwd);

    // 行内重命名:校验 name 非空 + hash 已知 → 原子写 workspace.json → 更新内存。
    // name 空、hash 未知、写盘失败均返回 false 并保持内存 cache 不变。
    bool set_name(const std::string& projects_dir, const std::string& hash, const std::string& name);

    // 测试 hook:让单测注入空 registry(scan 一个临时目录),不必涉及共享状态。

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, WorkspaceMeta> entries_;
};

// 确保某 cwd 的 workspace.json 存在。若文件已存在,不读取也不覆盖,避免
// TUI/daemon 启动把用户在 Desktop 里改过的 name 冲掉。
bool ensure_workspace_metadata(const std::string& projects_dir, const std::string& cwd);

// 默认命名策略 — 暴露为公共符号便于直接单测。
//   1. fs::path(cwd).filename() 非空 → 用它
//   2. 否则 fs::path(cwd).root_name()(处理 "C:\\" / "/")
//   3. 否则字面常量 "workspace"
std::string default_workspace_name(const std::string& cwd);

// 校验 workspace hash 是否与 cwd 的规范化 hash 一致。hash 为空或 cwd 为空
// 视为不一致;用于共享 daemon 的 workspace-scoped API 入参防错。
bool workspace_hash_matches_cwd(const std::string& hash, const std::string& cwd);

} // namespace acecode::desktop
