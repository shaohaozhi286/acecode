#include <gtest/gtest.h>

#include "headless/headless_mode.hpp"
#include "tool/ask_user_question_tool.hpp"
#include "tool/tool_executor.hpp"

#include <nlohmann/json.hpp>

namespace {

// headless 全局标记是进程级状态,测试之间必须复位,否则本文件的用例会
// 污染同进程内其它测试(比如 daemon ask prompter 相关用例)。
class HeadlessAskResultTest : public ::testing::Test {
protected:
    void TearDown() override { acecode::headless::set_active(false); }
};

// 一份合法的 AskUserQuestion 参数(单问题双选项)。
const char* kValidArgs = R"json({
    "questions": [{
        "question": "Which approach?",
        "header": "Approach",
        "options": [
            {"label": "A", "description": "plan A"},
            {"label": "B", "description": "plan B"}
        ]
    }]
})json";

} // namespace

// 场景:headless(-p)进程内,模型调用 AskUserQuestion。此进程没有任何
// 交互通道(无 TUI overlay / 无浏览器 WS),按 daemon 路径的 AsyncPrompter
// 走会空等 5 分钟超时。回归背景:openspec add-headless-print-mode 引入
// headless::active() 探针,工具必须在进 prompter 之前短路自动应答。
// 期望:success=true(避免模型当失败反复重问),文案含 "[Headless mode]"
// 且指示模型自行决策;ctx.ask_user_questions 完全不被调用。
TEST_F(HeadlessAskResultTest, AutoAnswersWithoutTouchingPrompter) {
    acecode::headless::set_active(true);

    auto tool = acecode::create_ask_user_question_tool_async();
    acecode::ToolContext ctx;
    bool prompter_called = false;
    ctx.ask_user_questions = [&](const nlohmann::json&) -> nlohmann::json {
        prompter_called = true;
        return {{"cancelled", true}};
    };

    auto result = tool.execute(kValidArgs, ctx);

    EXPECT_TRUE(result.success);
    EXPECT_NE(result.output.find("[Headless mode]"), std::string::npos);
    EXPECT_FALSE(prompter_called);
}

// 场景:同一个工具在非 headless 进程里(TUI/daemon 正常路径),
// ask_user_questions 通道缺失。
// 期望:维持原有行为 —— 返回"工具不可用"的失败结果,证明 headless 分支
// 没有改变默认路径(回归保护:headless::active() 默认 false)。
TEST_F(HeadlessAskResultTest, NormalModeStillReportsUnavailableChannel) {
    acecode::headless::set_active(false);

    auto tool = acecode::create_ask_user_question_tool_async();
    acecode::ToolContext ctx; // ask_user_questions 留空

    auto result = tool.execute(kValidArgs, ctx);

    EXPECT_FALSE(result.success);
    EXPECT_NE(result.output.find("not supported"), std::string::npos);
}

// 场景:headless 与 goal unattended 同时成立(-p 进程里恰好有 active goal
// 的父会话探针)。两个分支都会自动应答,但对模型的环境解释不同。
// 期望:headless 分支优先(工具实现里 headless 判定在 goal 之前),文案是
// print 模式的解释而非 goal 的 —— 模型据此知道"整个进程都无人值守",
// 而不是误以为只是 goal 期间暂时无人。
TEST_F(HeadlessAskResultTest, HeadlessBranchWinsOverGoalUnattended) {
    acecode::headless::set_active(true);

    auto tool = acecode::create_ask_user_question_tool_async();
    acecode::ToolContext ctx;
    ctx.goal_unattended_active = [] { return true; };

    auto result = tool.execute(kValidArgs, ctx);

    EXPECT_TRUE(result.success);
    EXPECT_NE(result.output.find("[Headless mode]"), std::string::npos);
    EXPECT_EQ(result.output.find("[Unattended goal mode]"), std::string::npos);
}

// 场景:make_headless_ask_result 与 make_goal_unattended_ask_result 的
// 文案契约 —— 两者都必须 success=true 且都包含"不要再问、自行决策"的
// 指示(阈值说明:success=false 会让部分模型把自动应答当工具故障,进入
// 重试循环,这是 goal unattended 落地时实测过的行为)。
TEST_F(HeadlessAskResultTest, ResultContractsShareAutonomousInstruction) {
    auto headless = acecode::make_headless_ask_result();
    EXPECT_TRUE(headless.success);
    EXPECT_NE(headless.output.find("Do not wait and do not ask again"),
              std::string::npos);

    auto goal = acecode::make_goal_unattended_ask_result();
    EXPECT_TRUE(goal.success);
    EXPECT_NE(goal.output.find("Do not wait and do not ask again"),
              std::string::npos);
}
