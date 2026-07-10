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

    // --question-policy <ask|deny|timeout[:秒数]>(add-ask-question-policy)。
    // question_policy 为空 = 未指定;question_policy_error 非空 = 解析失败,
    // 调用方应报错退出(fail fast,CLI 是显式意志不做静默归一化)。
    // question_timeout_seconds 仅在 "timeout:N" 冒号语法给出且合法时非 0。
    std::string question_policy;
    int question_timeout_seconds = 0;
    std::string question_policy_error;

    bool direct_resume_requested() const {
        return resume_latest || !resume_session_id.empty();
    }
};

InteractiveCliOptions parse_interactive_cli_options(int argc, char* argv[]);

// 解析 "--question-policy" 的取值("ask" / "deny" / "timeout" /
// "timeout:N",N ∈ [5, 3600])。成功返回 true 并填 policy / timeout_seconds
// (无冒号秒数时 timeout_seconds 置 0);失败返回 false 并填 error。
// 抽成独立函数供 TUI 与 daemon CLI 共用 + 单测直接覆盖。
bool parse_question_policy_value(const std::string& value,
                                 std::string& policy,
                                 int& timeout_seconds,
                                 std::string& error);

} // namespace acecode
