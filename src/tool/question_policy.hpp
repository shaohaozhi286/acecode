#pragma once

// AskUserQuestion 应答策略解析(openspec/changes/add-ask-question-policy)。
//
// 纯函数层,无 UI / config IO 依赖,进 acecode_testable。TUI 与 daemon 两路
// AskUserQuestion 实现共用同一份解析结果,保证双端行为一致。
//
// active goal 的 AskUserQuestion 在工具入口覆盖为固定 30 秒
// Timeout，不属于配置解析函数的职责。

#include <string>

namespace acecode {

inline constexpr int kGoalQuestionTimeoutSeconds = 30;

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
    //   "goal"          = active goal 固定 30 秒 timeout 覆盖
    //   "default"       = 无任何配置,默认 ask
    const char* origin = "default";
};

// 解析生效策略。优先级(高→低):
//   1. 显式配置(policy_explicit=true)按面值生效
//   2. 默认 Ask
//
// configured_policy 传归一化后的配置值("ask"/"deny"/"timeout";load_config
// 已把非法值归一化,这里对意外值防御性回退 Ask)。
// permission mode 不参与提问策略解析；YOLO 只控制工具权限确认。
// timeout_seconds 越界时防御性归位 60(load_config/CLI 已 clamp,这里兜底)。
ResolvedQuestionPolicy resolve_question_policy(
    const std::string& configured_policy,
    bool policy_explicit,
    int configured_timeout_seconds);

} // namespace acecode
