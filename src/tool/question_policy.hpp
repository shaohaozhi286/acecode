#pragma once

// AskUserQuestion 应答策略解析(openspec/changes/add-ask-question-policy)。
//
// 纯函数层,无 UI / config IO 依赖,进 acecode_testable。TUI 与 daemon 两路
// AskUserQuestion 实现共用同一份解析结果,保证双端行为一致。
//
// 注意:goal 无人值守豁免不在这里 —— 工具入口的 goal_unattended(ctx) 分支
// 优先级更高(返回 goal 专属自动应答文案),先于本函数被检查。

#include <string>

namespace acecode {

enum class QuestionPolicy {
    Ask,     // 正常弹 UI 无限期等回答(默认,现行行为)
    Deny,    // 不弹 UI,立即自动应答让模型自行决策
    Timeout, // 弹 UI 等 N 秒,无人回答自动采纳每题第一个选项
};

struct ResolvedQuestionPolicy {
    QuestionPolicy policy = QuestionPolicy::Ask;
    int timeout_seconds = 60;   // 仅 policy=Timeout 时有意义
    // 解析来源,进日志与 ToolResult metadata:
    //   "explicit"      = config 或 CLI 显式指定
    //   "yolo-implicit" = YOLO 权限模式的隐式 deny 映射
    //   "default"       = 无任何配置,默认 ask
    const char* origin = "default";
};

// 解析生效策略。优先级(高→低):
//   1. 显式配置(policy_explicit=true)按面值生效
//   2. permission_mode == "yolo" 且未显式配置 → Deny
//   3. 默认 Ask
//
// configured_policy 传归一化后的配置值("ask"/"deny"/"timeout";load_config
// 已把非法值归一化,这里对意外值防御性回退 Ask)。permission_mode 传
// ToolContext::current_permission_mode() 的返回值(YOLO/dangerous 时为
// "yolo")。timeout_seconds 越界时防御性归位 60(load_config/CLI 已 clamp,
// 这里兜底)。
ResolvedQuestionPolicy resolve_question_policy(
    const std::string& configured_policy,
    bool policy_explicit,
    int configured_timeout_seconds,
    const std::string& permission_mode);

} // namespace acecode
