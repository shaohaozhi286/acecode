#include "worktree_tool.hpp"

#include "../session/session_manager.hpp"
#include "../utils/utf8_path.hpp"
#include "../worktree/worktree_core.hpp"
#include "../worktree/worktree_manager.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <random>
#include <sstream>
#include <system_error>

namespace acecode {

namespace {

ToolResult tool_error(const std::string& message) {
    return ToolResult{"[Error] " + message, false};
}

ToolDef enter_worktree_def() {
    ToolDef def;
    def.name = "EnterWorktree";
    def.description =
        "Use this tool ONLY when the user explicitly asks to work in a worktree. "
        "It creates an isolated git worktree and switches the current session into it.\n"
        "\n"
        "When to use: the user explicitly says \"worktree\" (e.g. \"start a worktree\", "
        "\"work in a worktree\", \"create a worktree\").\n"
        "When NOT to use: the user asks to create or switch branches (use git commands "
        "instead), or asks to fix a bug / build a feature without mentioning worktrees. "
        "Never call this tool unless the user explicitly mentions \"worktree\".\n"
        "\n"
        "Requirements: must be in a git repository; must not already be in a worktree "
        "session.\n"
        "Behavior: creates a git worktree inside .acecode/worktrees/ of the main "
        "repository on a new branch based on origin/<default-branch> (falls back to "
        "the current HEAD when the remote branch is unavailable), then switches the "
        "session's working directory into it. Use ExitWorktree to leave (keep or "
        "remove).";
    def.parameters = {
        {"type", "object"},
        {"properties", {
            {"name", {
                {"type", "string"},
                {"description",
                 "Optional name for the worktree. Each \"/\"-separated segment may "
                 "contain only letters, digits, dots, underscores, and dashes; max 64 "
                 "chars total. A random name is generated if not provided."}
            }}
        }},
        {"additionalProperties", false},
    };
    return def;
}

ToolDef exit_worktree_def() {
    ToolDef def;
    def.name = "ExitWorktree";
    def.description =
        "Exit a worktree session created by EnterWorktree and return the session to "
        "the original working directory.\n"
        "\n"
        "Scope: only operates on the worktree entered by EnterWorktree in this "
        "session. It will NOT touch worktrees created manually with `git worktree "
        "add` or by a previous session. Called outside a worktree session it is a "
        "no-op that changes nothing on disk.\n"
        "\n"
        "When to use: the user explicitly asks to exit / leave the worktree or go "
        "back. Do NOT call this proactively.\n"
        "\n"
        "action \"keep\" leaves the worktree directory and branch on disk; \"remove\" "
        "deletes both. If the worktree has uncommitted files or commits not on the "
        "original branch, remove is refused unless discard_changes is true — confirm "
        "with the user before re-invoking with discard_changes: true.";
    def.parameters = {
        {"type", "object"},
        {"required", nlohmann::json::array({"action"})},
        {"properties", {
            {"action", {
                {"type", "string"},
                {"enum", nlohmann::json::array({"keep", "remove"})},
                {"description",
                 "\"keep\" leaves the worktree and branch on disk; \"remove\" deletes "
                 "both."}
            }},
            {"discard_changes", {
                {"type", "boolean"},
                {"description",
                 "Required true when action is \"remove\" and the worktree has "
                 "uncommitted files or unmerged commits. The tool will refuse and "
                 "list them otherwise."}
            }}
        }},
        {"additionalProperties", false},
    };
    return def;
}

ToolResult execute_enter_worktree(const std::string& arguments_json,
                                  const ToolContext& ctx,
                                  const WorktreeConfig& cfg) {
    if (!ctx.session_manager) {
        return tool_error("EnterWorktree requires an active session");
    }
    if (!ctx.switch_session_cwd) {
        return tool_error("EnterWorktree is not supported in this runtime");
    }
    if (ctx.session_manager->active_worktree().active()) {
        return tool_error("Already in a worktree session");
    }
    if (ctx.cwd.empty()) {
        return tool_error("EnterWorktree requires a session working directory");
    }

    std::string slug;
    try {
        auto args = nlohmann::json::parse(arguments_json.empty() ? "{}" : arguments_json);
        if (args.contains("name") && args["name"].is_string()) {
            slug = args["name"].get<std::string>();
        }
    } catch (const std::exception& e) {
        return tool_error(std::string("Invalid arguments: ") + e.what());
    }

    if (slug.empty()) {
        slug = worktree::generate_worktree_slug(std::random_device{}());
    }
    if (std::string err = worktree::validate_worktree_slug(slug); !err.empty()) {
        return tool_error(err);
    }

    // 从主仓根创建 —— 会话已经在某个 worktree 里(手工 cd 进去等)时也
    // 落在主仓的 .acecode/worktrees/ 下,不嵌套。
    const std::string repo_root = worktree::find_canonical_git_root(ctx.cwd);
    if (repo_root.empty()) {
        return tool_error(
            "Cannot create a worktree: " + ctx.cwd + " is not in a git repository");
    }

    worktree::WorktreeCreateOptions options;
    options.sparse_paths = cfg.sparse_paths;
    auto created = worktree::get_or_create_worktree(repo_root, slug, options);
    if (!created.ok) {
        return tool_error(created.error);
    }
    if (!created.existed) {
        worktree::PostCreationOptions post;
        post.symlink_directories = cfg.symlink_directories;
        worktree::perform_post_creation_setup(repo_root, created.worktree_path, post);
    }

    WorktreeSessionInfo info;
    info.original_cwd = ctx.cwd;
    info.worktree_path = created.worktree_path;
    info.worktree_name = slug;
    info.worktree_branch = created.worktree_branch;
    info.original_head_commit = created.head_commit;
    ctx.session_manager->set_active_worktree(info);
    ctx.switch_session_cwd(created.worktree_path);

    std::ostringstream oss;
    oss << (created.existed ? "Resumed existing worktree at " : "Created worktree at ")
        << created.worktree_path << " on branch " << created.worktree_branch;
    if (!created.existed && !created.base_ref.empty()) {
        oss << " (based on " << created.base_ref << ")";
    }
    oss << ". The session is now working in the worktree. "
        << "Use ExitWorktree to leave (keep or remove).";

    ToolResult result{oss.str(), true};
    ToolSummary summary;
    summary.verb = created.existed ? "Resumed" : "Created";
    summary.object = "worktree " + slug;
    summary.metrics.push_back({"branch", created.worktree_branch});
    summary.icon = "\xF0\x9F\x8C\xB3"; // 🌳
    result.summary = summary;
    return result;
}

ToolResult execute_exit_worktree(const std::string& arguments_json,
                                 const ToolContext& ctx) {
    if (!ctx.session_manager) {
        return tool_error("ExitWorktree requires an active session");
    }
    const WorktreeSessionInfo info = ctx.session_manager->active_worktree();
    if (!info.active()) {
        // no-op 语义(与 Claude Code 一致):明确告知没有可退出的 worktree
        // 会话,且没有任何文件系统改动 —— 手工建的 worktree 不归它管。
        return tool_error(
            "No-op: there is no active EnterWorktree session to exit. This tool only "
            "operates on worktrees created by EnterWorktree in the current session — "
            "it will not touch worktrees created manually or in a previous session. "
            "No filesystem changes were made.");
    }
    if (!ctx.switch_session_cwd) {
        return tool_error("ExitWorktree is not supported in this runtime");
    }

    std::string action;
    bool discard_changes = false;
    try {
        auto args = nlohmann::json::parse(arguments_json.empty() ? "{}" : arguments_json);
        if (args.contains("action") && args["action"].is_string()) {
            action = args["action"].get<std::string>();
        }
        if (args.contains("discard_changes") && args["discard_changes"].is_boolean()) {
            discard_changes = args["discard_changes"].get<bool>();
        }
    } catch (const std::exception& e) {
        return tool_error(std::string("Invalid arguments: ") + e.what());
    }
    if (action != "keep" && action != "remove") {
        return tool_error("action must be \"keep\" or \"remove\"");
    }

    // 变更计数既做 remove 的安全门,也进最终消息。fail-closed:数不清
    // (git 失败 / 缺基线)时按"未知,视为有变更"处理。
    const auto summary =
        worktree::count_worktree_changes(info.worktree_path, info.original_head_commit);

    if (action == "remove" && !discard_changes) {
        if (!summary.has_value()) {
            return tool_error(
                "Could not verify worktree state at " + info.worktree_path +
                ". Refusing to remove without explicit confirmation. Re-invoke with "
                "discard_changes: true to proceed — or use action: \"keep\" to "
                "preserve the worktree.");
        }
        if (summary->changed_files > 0 || summary->commits > 0) {
            std::ostringstream oss;
            oss << "Worktree has ";
            if (summary->changed_files > 0) {
                oss << summary->changed_files << " uncommitted "
                    << (summary->changed_files == 1 ? "file" : "files");
                if (summary->commits > 0) oss << " and ";
            }
            if (summary->commits > 0) {
                oss << summary->commits << (summary->commits == 1 ? " commit" : " commits")
                    << " on " << (info.worktree_branch.empty() ? "the worktree branch"
                                                               : info.worktree_branch);
            }
            oss << ". Removing will discard this work permanently. Confirm with the "
                   "user, then re-invoke with discard_changes: true — or use action: "
                   "\"keep\" to preserve the worktree.";
            return tool_error(oss.str());
        }
    }

    // 先切回原目录再动 worktree:remove 会删掉当前所在目录。
    ctx.switch_session_cwd(info.original_cwd);
    ctx.session_manager->clear_active_worktree();

    if (action == "keep") {
        std::ostringstream oss;
        oss << "Exited worktree. Your work is preserved at " << info.worktree_path;
        if (!info.worktree_branch.empty()) {
            oss << " on branch " << info.worktree_branch;
        }
        oss << ". Session is now back in " << info.original_cwd << ".";
        ToolResult result{oss.str(), true};
        ToolSummary s;
        s.verb = "Exited";
        s.object = "worktree " + info.worktree_name;
        s.metrics.push_back({"kept", info.worktree_path});
        result.summary = s;
        return result;
    }

    // action == "remove":从主仓根执行删除(worktree 目录马上就没了)。
    // --worktree 启动的 TUI 进程 cwd 就在 worktree 里 —— Windows 上删除
    // 进程当前目录会失败,先把进程 cwd 挪回原目录(daemon 进程 cwd 不在
    // worktree 里,此分支天然不触发)。
    {
        std::error_code cwd_ec;
        const auto process_cwd = std::filesystem::current_path(cwd_ec);
        if (!cwd_ec) {
            const std::string process_cwd_utf8 = path_to_utf8(process_cwd);
            if (process_cwd_utf8.rfind(info.worktree_path, 0) == 0) {
                std::filesystem::current_path(path_from_utf8(info.original_cwd), cwd_ec);
            }
        }
    }
    std::string repo_root = worktree::find_canonical_git_root(info.original_cwd);
    if (repo_root.empty()) repo_root = info.original_cwd;
    std::string remove_error;
    const bool removed = worktree::remove_worktree(
        repo_root, info.worktree_path, info.worktree_branch, &remove_error);

    std::ostringstream oss;
    if (removed) {
        oss << "Exited and removed worktree at " << info.worktree_path << ".";
        const int commits = summary ? summary->commits : 0;
        const int changed = summary ? summary->changed_files : 0;
        if (commits > 0 || changed > 0) {
            oss << " Discarded";
            if (commits > 0) {
                oss << " " << commits << (commits == 1 ? " commit" : " commits");
                if (changed > 0) oss << " and";
            }
            if (changed > 0) {
                oss << " " << changed << " uncommitted "
                    << (changed == 1 ? "file" : "files");
            }
            oss << ".";
        }
        oss << " Session is now back in " << info.original_cwd << ".";
    } else {
        // 删除失败时会话状态已恢复(cwd 回原目录、状态已清),worktree
        // 目录留在磁盘上;报错让用户自行处理,不假装删掉了。
        oss << "Exited worktree but failed to remove it: " << remove_error
            << ". The worktree remains at " << info.worktree_path
            << ". Session is now back in " << info.original_cwd << ".";
    }

    ToolResult result{oss.str(), removed};
    ToolSummary s;
    s.verb = removed ? "Removed" : "Exited";
    s.object = "worktree " + info.worktree_name;
    result.summary = s;
    return result;
}

} // namespace

ToolImpl create_enter_worktree_tool(const WorktreeConfig& config) {
    ToolImpl impl;
    impl.definition = enter_worktree_def();
    impl.execute = [config](const std::string& arguments_json, const ToolContext& ctx) {
        return execute_enter_worktree(arguments_json, ctx, config);
    };
    impl.is_read_only = false;
    impl.source = ToolSource::Builtin;
    return impl;
}

ToolImpl create_exit_worktree_tool() {
    ToolImpl impl;
    impl.definition = exit_worktree_def();
    impl.execute = [](const std::string& arguments_json, const ToolContext& ctx) {
        return execute_exit_worktree(arguments_json, ctx);
    };
    impl.is_read_only = false;
    impl.source = ToolSource::Builtin;
    return impl;
}

} // namespace acecode
