// 覆盖 src/permissions.hpp 的决策逻辑:
//   - 三种模式(Default / AcceptEdits / Yolo)下哪些工具自动放行
//   - 用户自定义 PermissionRule(按 tool/path/command 匹配 + priority)
//   - 会话级 always-allow(/permissions 的 "always" 记忆)
// 任何回归都会让用户在 TUI 上要么多出现确认弹窗、要么危险命令被静默放行,
// 体感非常直接,所以这块测试用例密度意图高于别的模块。

#include <gtest/gtest.h>

#include "permissions.hpp"

using acecode::PermissionManager;
using acecode::PermissionMode;
using acecode::PermissionRule;
using acecode::RuleAction;

// 场景:Default 模式下,read-only 工具(is_read_only=true)自动放行,
// 写/执行工具(is_read_only=false)需要提示用户确认。
TEST(Permissions, DefaultModeAllowsReadOnlyTools) {
    PermissionManager pm;
    pm.set_mode(PermissionMode::Default);

    // Read-only: auto-allow without prompt.
    EXPECT_TRUE(pm.should_auto_allow("file_read", /*is_read_only=*/true));
    EXPECT_TRUE(pm.should_auto_allow("grep",      /*is_read_only=*/true));

    // Write/exec: require prompt in default mode.
    EXPECT_FALSE(pm.should_auto_allow("file_write", /*is_read_only=*/false));
    EXPECT_FALSE(pm.should_auto_allow("bash",       /*is_read_only=*/false));
}

// 场景:Yolo 模式所有工具自动放行,包括 bash、file_write 这类高风险项;
// 这是 --dangerous / yolo 模式下"不问直接跑"的契约。
TEST(Permissions, YoloModeAllowsAll) {
    PermissionManager pm;
    pm.set_mode(PermissionMode::Yolo);
    EXPECT_TRUE(pm.should_auto_allow("bash",       false));
    EXPECT_TRUE(pm.should_auto_allow("file_write", false));
    EXPECT_TRUE(pm.should_auto_allow("file_edit",  false));
}

// 场景:AcceptEdits 模式自动放行 file_write / file_edit,但仍然拦截 bash;
// 这个模式是为了让"大段写代码"不被反复打断,但危险命令(shell 执行)
// 还要用户确认,测试锁定这条边界。
TEST(Permissions, AcceptEditsModeAllowsFileWritesButNotBash) {
    PermissionManager pm;
    pm.set_mode(PermissionMode::AcceptEdits);
    EXPECT_TRUE(pm.should_auto_allow("file_write", false));
    EXPECT_TRUE(pm.should_auto_allow("file_edit",  false));
    EXPECT_FALSE(pm.should_auto_allow("bash",      false));
}

// 场景:添加一条针对 bash + command 前缀 "ls " 的 Allow 规则后,ls -la
// 自动放行,rm -rf / 不命中规则(走 Default 模式的常规确认)仍被拦截。
TEST(Permissions, GlobRuleMatchingAllowsMatchedCommand) {
    PermissionManager pm;
    pm.set_mode(PermissionMode::Default);

    PermissionRule rule;
    rule.tool_pattern = "bash";
    rule.command_pattern = "ls ";  // prefix match
    rule.action = RuleAction::Allow;
    rule.priority = 10;
    pm.add_rule(rule);

    EXPECT_TRUE(pm.should_auto_allow("bash", /*is_read_only=*/false,
                                     /*path=*/"", /*command=*/"ls -la"));
    EXPECT_FALSE(pm.should_auto_allow("bash", /*is_read_only=*/false,
                                      /*path=*/"", /*command=*/"rm -rf /"));
}

// 场景:规则的 tool_pattern 必须生效——针对 bash 的规则不能顺带把
// file_write 也一起放行,否则"只给某个工具加 allow"就失去语义。
TEST(Permissions, GlobRuleMatchingRespectsToolPattern) {
    PermissionManager pm;
    pm.set_mode(PermissionMode::Default);

    // Rule targets bash only — file_write should fall back to default prompt.
    PermissionRule rule;
    rule.tool_pattern = "bash";
    rule.action = RuleAction::Allow;
    rule.priority = 10;
    pm.add_rule(rule);

    EXPECT_TRUE(pm.should_auto_allow("bash",       false));
    EXPECT_FALSE(pm.should_auto_allow("file_write", false));
}

// 场景:即使在 Yolo 这种 baseline 全放行的模式下,一条高优先级的 Deny
// 规则仍能精准拦住 rm -rf;而未命中 Deny 的命令还是按 Yolo 放行。
// 这是"即使开 yolo,也要能黑名单少数危险命令"的守门机制。
TEST(Permissions, HigherPriorityDenyBeatsAllow) {
    PermissionManager pm;
    pm.set_mode(PermissionMode::Yolo);  // baseline allow-all

    // Explicit deny on a specific pattern must win over mode-based allow.
    PermissionRule deny;
    deny.tool_pattern = "bash";
    deny.command_pattern = "rm -rf";
    deny.action = RuleAction::Deny;
    deny.priority = 100;
    pm.add_rule(deny);

    EXPECT_FALSE(pm.should_auto_allow("bash", false, "", "rm -rf /"));
    // Commands that don't match the deny pattern still go through Yolo.
    EXPECT_TRUE(pm.should_auto_allow("bash", false, "", "echo hi"));
}

// 场景:用户在确认弹窗选"总是允许"后,该工具在后续会话内自动放行;
// clear_session_allows 调用后 sticky 状态清零——mode 切换时触发。
TEST(Permissions, SessionAllowStickyForTool) {
    PermissionManager pm;
    pm.set_mode(PermissionMode::Default);
    EXPECT_FALSE(pm.should_auto_allow("bash", false));
    pm.add_session_allow("bash");
    EXPECT_TRUE(pm.should_auto_allow("bash", false));
    EXPECT_TRUE(pm.has_session_allow("bash"));
    pm.clear_session_allows();
    EXPECT_FALSE(pm.has_session_allow("bash"));
}
