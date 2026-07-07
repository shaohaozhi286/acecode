#pragma once

#include <string>

namespace acecode {

struct InteractiveCliOptions {
    bool dangerous_mode = false;
    bool run_configure_cmd = false;
    bool validate_models_registry_cmd = false;
    bool resume_latest = false;
    bool resume_picker_on_startup = false;
    bool force_alt_screen = false;
    std::string resume_session_id;

    // --worktree [name] / -w [name] / --worktree=<name>:启动时先创建
    // (或复用)一个隔离 worktree,整个会话在里面工作。name 缺省时随机
    // 生成;name 也可以是 PR 引用(#123 / GitHub PR URL)→ 基于 PR head
    // 建 worktree,slug 记为 pr-<N>。
    bool worktree_enabled = false;
    std::string worktree_name;

    bool direct_resume_requested() const {
        return resume_latest || !resume_session_id.empty();
    }
};

InteractiveCliOptions parse_interactive_cli_options(int argc, char* argv[]);

} // namespace acecode
