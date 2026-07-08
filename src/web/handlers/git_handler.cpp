#include "git_handler.hpp"

#include "../../gitinfo/git_context_collector.hpp"
#include "../../gitinfo/git_context_core.hpp"
#include "files_handler.hpp"

#include <filesystem>
#include <variant>

namespace acecode::web {

GitApiResponse build_git_info_payload(const std::string& cwd,
                                      const std::vector<std::string>& allowed_cwds,
                                      bool enabled,
                                      int timeout_ms) {
    GitApiResponse resp;

    auto validated = validate_path_within(cwd, "", allowed_cwds);
    if (std::holds_alternative<FileError>(validated)) {
        resp.status = 400;
        resp.body = {{"error", "unknown workspace"}};
        return resp;
    }

    if (!enabled) {
        // disabled 时按非仓库处理:前端拿到 is_repo=false 自然隐藏一切
        // git UI,且不 spawn 任何 git 子进程。
        resp.body = {{"is_repo", false}};
        return resp;
    }

    gitinfo::GitInfo info = gitinfo::collect_git_info(cwd, timeout_ms);
    if (!info.is_repo) {
        resp.body = {{"is_repo", false}};
        return resp;
    }

    resp.body = {
        {"is_repo", true},
        {"branch", info.branch},
        {"default_branch", info.default_branch},
        {"branches", info.branches},
        {"dirty", info.dirty},
    };
    return resp;
}

GitApiResponse build_git_checkout_payload(
    const std::string& cwd,
    const std::string& branch,
    bool stash,
    const std::vector<std::string>& allowed_cwds,
    bool enabled,
    int timeout_ms,
    const std::function<bool()>& is_workspace_busy) {
    GitApiResponse resp;

    auto validated = validate_path_within(cwd, "", allowed_cwds);
    if (std::holds_alternative<FileError>(validated)) {
        resp.status = 400;
        resp.body = {{"error", "unknown workspace"}};
        return resp;
    }
    if (!enabled) {
        resp.status = 400;
        resp.body = {{"error", "git context disabled"}};
        return resp;
    }
    if (!gitinfo::is_safe_ref_name(branch)) {
        resp.status = 400;
        resp.body = {{"error", "invalid branch name"}};
        return resp;
    }
    // busy 门在任何 git 副作用之前:agent 写文件写一半被切分支是数据灾难。
    if (is_workspace_busy && is_workspace_busy()) {
        resp.status = 409;
        resp.body = {{"error", "busy"}};
        return resp;
    }
    if (!gitinfo::is_inside_git_repo(cwd)) {
        resp.status = 400;
        resp.body = {{"error", "not a git repository"}};
        return resp;
    }

    auto changes = gitinfo::list_tracked_changes(cwd, timeout_ms);
    if (!changes.query_ok) {
        resp.status = 500;
        resp.body = {{"error", "failed to query working tree state"}};
        return resp;
    }
    if (!changes.files.empty() && !stash) {
        // 409 往返式确认(design D3):前端据此弹 stash 确认框后带
        // stash:true 重发。daemon 是唯一可信判定点,前端不预判。
        resp.status = 409;
        resp.body = {{"error", "dirty"}, {"files", changes.files}};
        return resp;
    }
    if (!changes.files.empty() && stash) {
        auto stashed = gitinfo::stash_all_changes(cwd, timeout_ms);
        if (!stashed.ok) {
            resp.status = 500;
            resp.body = {{"error", "stash failed"}, {"detail", stashed.error}};
            return resp;
        }
    }

    auto checked_out = gitinfo::checkout_branch(cwd, branch, timeout_ms);
    if (!checked_out.ok) {
        // stash 后 checkout 仍可能失败(如 untracked 冲突);git 错误原样
        // 透传,改动已在 stash 里不会丢。
        resp.status = 409;
        resp.body = {{"error", "checkout failed"}, {"detail", checked_out.error}};
        return resp;
    }

    resp.body = {{"ok", true}, {"branch", branch}};
    return resp;
}

namespace {

// list/diff 共用的错误分派:collector 的 error_kind → HTTP 语义。
GitApiResponse changes_error_response(const std::string& error_kind) {
    GitApiResponse resp;
    if (error_kind == "invalid_base") {
        resp.status = 400;
        resp.body = {{"error", "invalid base"}};
    } else if (error_kind == "timeout") {
        resp.status = 504;
        resp.body = {{"error", "git timeout"}};
    } else if (error_kind == "too_large") {
        resp.status = 413;
        resp.body = {{"error", "diff too large"}};
    } else {
        resp.status = 500;
        resp.body = {{"error", "git error"}};
    }
    return resp;
}

} // namespace

GitApiResponse build_git_changes_payload(
    const std::string& cwd,
    const std::string& base,
    const std::vector<std::string>& allowed_cwds,
    bool enabled,
    int timeout_ms) {
    GitApiResponse resp;

    auto validated = validate_path_within(cwd, "", allowed_cwds);
    if (std::holds_alternative<FileError>(validated)) {
        resp.status = 400;
        resp.body = {{"error", "unknown workspace"}};
        return resp;
    }
    if (!enabled) {
        resp.status = 400;
        resp.body = {{"error", "git context disabled"}};
        return resp;
    }
    if (base.empty()) {
        resp.status = 400;
        resp.body = {{"error", "base parameter required"}};
        return resp;
    }

    gitinfo::GitChangesList list =
        gitinfo::list_git_changes(cwd, base, timeout_ms);
    if (!list.ok) return changes_error_response(list.error_kind);

    nlohmann::json files = nlohmann::json::array();
    for (const auto& f : list.files) {
        nlohmann::json item = {{"path", f.path}, {"status", f.status}};
        if (f.binary) item["binary"] = true;
        if (f.additions >= 0) item["additions"] = f.additions;
        if (f.deletions >= 0) item["deletions"] = f.deletions;
        files.push_back(std::move(item));
    }
    resp.body = {
        {"branch", list.branch},
        {"base", list.base},
        {"files", std::move(files)},
        {"total_additions", list.total_additions},
        {"total_deletions", list.total_deletions},
        {"total_count", list.total_count},
        {"truncated", list.truncated},
    };
    return resp;
}

GitApiResponse build_git_file_diff_payload(
    const std::string& cwd,
    const std::string& base,
    const std::string& path,
    const std::vector<std::string>& allowed_cwds,
    bool enabled,
    int timeout_ms) {
    GitApiResponse resp;

    // path 走与 /api/files 相同的 canonical + 前缀校验,拒绝仓库外路径。
    auto validated = validate_path_within(cwd, path, allowed_cwds);
    if (std::holds_alternative<FileError>(validated)) {
        resp.status = 400;
        resp.body = {{"error", "unknown workspace or path outside workspace"}};
        return resp;
    }
    if (!enabled) {
        resp.status = 400;
        resp.body = {{"error", "git context disabled"}};
        return resp;
    }
    if (base.empty() || path.empty()) {
        resp.status = 400;
        resp.body = {{"error", "base and path parameters required"}};
        return resp;
    }

    gitinfo::GitFileDiff diff =
        gitinfo::get_file_diff(cwd, base, path, timeout_ms);
    if (!diff.ok) return changes_error_response(diff.error_kind);

    resp.body = {{"patch", diff.patch}};
    return resp;
}

} // namespace acecode::web
