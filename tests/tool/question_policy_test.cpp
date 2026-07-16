// resolve_question_policy 纯函数的全分支覆盖
// (openspec/changes/add-ask-question-policy)。
//
// 优先级契约:显式配置 > 默认 Ask。permission mode 不参与
// 提问策略解析;active goal 的 Timeout(30) 在工具入口覆盖。

#include <gtest/gtest.h>

#include "tool/question_policy.hpp"

using namespace acecode;

// 场景:无任何配置 + 普通权限模式 → 默认 Ask
TEST(QuestionPolicyResolve, DefaultIsAsk) {
    auto r = resolve_question_policy("ask", false, 60);
    EXPECT_EQ(r.policy, QuestionPolicy::Ask);
    EXPECT_STREQ(r.origin, "default");
}

// 场景:显式 deny / timeout 按面值生效。
TEST(QuestionPolicyResolve, ExplicitValuesTakenAtFaceValue) {
    {
        auto r = resolve_question_policy("deny", true, 60);
        EXPECT_EQ(r.policy, QuestionPolicy::Deny);
        EXPECT_STREQ(r.origin, "explicit");
    }
    {
        auto r = resolve_question_policy("timeout", true, 120);
        EXPECT_EQ(r.policy, QuestionPolicy::Timeout);
        EXPECT_EQ(r.timeout_seconds, 120);
        EXPECT_STREQ(r.origin, "explicit");
    }
}

// 场景:意外的策略字符串防御性回退 Ask(load_config 已归一化,这里兜底)
TEST(QuestionPolicyResolve, UnknownExplicitValueFallsBackToAsk) {
    auto r = resolve_question_policy("banana", true, 60);
    EXPECT_EQ(r.policy, QuestionPolicy::Ask);
}

// 场景:越界超时秒数防御性归位 60;合法边界值原样保留
TEST(QuestionPolicyResolve, TimeoutSecondsSanitized) {
    EXPECT_EQ(resolve_question_policy("timeout", true, 2).timeout_seconds, 60);
    EXPECT_EQ(resolve_question_policy("timeout", true, 4000).timeout_seconds, 60);
    EXPECT_EQ(resolve_question_policy("timeout", true, 5).timeout_seconds, 5);
    EXPECT_EQ(resolve_question_policy("timeout", true, 3600).timeout_seconds, 3600);
}

// YOLO 不再传入 resolver;无显式策略时所有权限模式共用 Ask。
TEST(QuestionPolicyResolve, PermissionModeDoesNotAlterDefaultAsk) {
    auto r = resolve_question_policy("ask", false, 60);
    EXPECT_EQ(r.policy, QuestionPolicy::Ask);
    EXPECT_STREQ(r.origin, "default");
}
