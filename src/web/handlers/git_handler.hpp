#pragma once

// /api/git/* 的纯函数业务逻辑(openspec add-git-context)。
// 不依赖 Crow;routes_git.cpp 只做 HTTP 解析 + 序列化,方便 gtest 直测。
//
// 安全:cwd 复用 files_handler 的 allowed_cwds 白名单校验
// (validate_path_within),防止 daemon 被当成任意路径的 git 查询器。
// enabled=false(config.git_context.enabled)时按非仓库处理,不 spawn git。

#include <nlohmann/json.hpp>
#include <functional>
#include <string>
#include <vector>

namespace acecode::web {

struct GitApiResponse {
    int status = 200;
    nlohmann::json body;
};

// GET /api/git/info?cwd=<abs>
// 200: {is_repo, branch, default_branch, branches[], dirty}(非仓库只有 is_repo)
// 400: {"error":"unknown workspace"} — cwd 不在白名单
GitApiResponse build_git_info_payload(const std::string& cwd,
                                      const std::vector<std::string>& allowed_cwds,
                                      bool enabled,
                                      int timeout_ms);

// POST /api/git/checkout body {cwd, branch, stash}
// (openspec add-webui-git-session-pill)
// 安全门顺序:白名单 400 → disabled 400 → 分支名校验 400 → busy 409 →
// 非仓库 400 → dirty(tracked 且未带 stash)409 带 files → stash(可选)→
// checkout。成功 200 {ok:true, branch};git 失败 409/500 透传 stderr。
// is_workspace_busy:调用方注入的 workspace busy 探测(SessionRegistry)。
GitApiResponse build_git_checkout_payload(
    const std::string& cwd,
    const std::string& branch,
    bool stash,
    const std::vector<std::string>& allowed_cwds,
    bool enabled,
    int timeout_ms,
    const std::function<bool()>& is_workspace_busy);

// GET /api/git/changes?cwd=<abs>&base=<ref>
// (openspec redesign-sidepanel-git-changes)
// 200: {branch, base, files:[{path,status,additions?,deletions?,binary?}],
//       total_additions, total_deletions, total_count, truncated}
// 400: unknown workspace / invalid base / disabled;504: 超时。
// 列表 cap 200 条(truncated 标记),diff 正文由 /api/git/diff 按需拉。
// timeout 放宽为 2×(numstat 对大 diff 比 status 重)。
GitApiResponse build_git_changes_payload(
    const std::string& cwd,
    const std::string& base,
    const std::vector<std::string>& allowed_cwds,
    bool enabled,
    int timeout_ms);

// GET /api/git/diff?cwd=<abs>&path=<rel>&base=<ref>
// 200: {patch};400: 越权/invalid base;413: patch > 1MB;504: 超时。
GitApiResponse build_git_file_diff_payload(
    const std::string& cwd,
    const std::string& base,
    const std::string& path,
    const std::vector<std::string>& allowed_cwds,
    bool enabled,
    int timeout_ms);

} // namespace acecode::web
