// resolve_question_policy 纯函数的全分支覆盖
// (openspec/changes/add-ask-question-policy)。
//
// 优先级契约:显式配置 > yolo 隐式 Deny > 默认 Ask。
// goal 无人值守豁免不经过本函数(工具入口更早分支),这里不覆盖。

#include <gtest/gtest.h>

#include "tool/question_policy.hpp"

using namespace acecode;

// 场景:无任何配置 + 普通权限模式 → 默认 Ask
TEST(QuestionPolicyResolve, DefaultIsAsk) {
    auto r = resolve_question_policy("ask", false, 60, "default");
    EXPECT_EQ(r.policy, QuestionPolicy::Ask);
    EXPECT_STREQ(r.origin, "default");
}

// 场景:YOLO 且未显式配置 → 隐式映射 Deny
TEST(QuestionPolicyResolve, YoloImplicitlyMapsToDeny) {
    auto r = resolve_question_policy("ask", false, 60, "yolo");
    EXPECT_EQ(r.policy, QuestionPolicy::Deny);
    EXPECT_STREQ(r.origin, "yolo-implicit");
}

// 场景:显式配置 ask 压制 YOLO 隐式映射(用户显式意志赢)
TEST(QuestionPolicyResolve, ExplicitAskBeatsYolo) {
    auto r = resolve_question_policy("ask", true, 60, "yolo");
    EXPECT_EQ(r.policy, QuestionPolicy::Ask);
    EXPECT_STREQ(r.origin, "explicit");
}

// 场景:显式 deny / timeout 按面值生效(不管权限模式)
TEST(QuestionPolicyResolve, ExplicitValuesTakenAtFaceValue) {
    {
        auto r = resolve_question_policy("deny", true, 60, "default");
        EXPECT_EQ(r.policy, QuestionPolicy::Deny);
        EXPECT_STREQ(r.origin, "explicit");
    }
    {
        auto r = resolve_question_policy("timeout", true, 120, "acceptEdits");
        EXPECT_EQ(r.policy, QuestionPolicy::Timeout);
        EXPECT_EQ(r.timeout_seconds, 120);
        EXPECT_STREQ(r.origin, "explicit");
    }
    {
        // YOLO + 显式 timeout:显式赢,不降级成 deny
        auto r = resolve_question_policy("timeout", true, 30, "yolo");
        EXPECT_EQ(r.policy, QuestionPolicy::Timeout);
        EXPECT_EQ(r.timeout_seconds, 30);
    }
}

// 场景:意外的策略字符串防御性回退 Ask(load_config 已归一化,这里兜底)
TEST(QuestionPolicyResolve, UnknownExplicitValueFallsBackToAsk) {
    auto r = resolve_question_policy("banana", true, 60, "default");
    EXPECT_EQ(r.policy, QuestionPolicy::Ask);
}

// 场景:越界超时秒数防御性归位 60;合法边界值原样保留
TEST(QuestionPolicyResolve, TimeoutSecondsSanitized) {
    EXPECT_EQ(resolve_question_policy("timeout", true, 2, "default").timeout_seconds, 60);
    EXPECT_EQ(resolve_question_policy("timeout", true, 4000, "default").timeout_seconds, 60);
    EXPECT_EQ(resolve_question_policy("timeout", true, 5, "default").timeout_seconds, 5);
    EXPECT_EQ(resolve_question_policy("timeout", true, 3600, "default").timeout_seconds, 3600);
}

// 场景:plan / acceptEdits 等非 yolo 模式不触发隐式映射
TEST(QuestionPolicyResolve, NonYoloModesStayAsk) {
    for (const char* mode : {"default", "acceptEdits", "plan"}) {
        auto r = resolve_question_policy("ask", false, 60, mode);
        EXPECT_EQ(r.policy, QuestionPolicy::Ask) << mode;
        EXPECT_STREQ(r.origin, "default") << mode;
    }
}
