// 真实 jsonl 重放冒烟测试:把用户报告"resume 工具调用空白"那次会话的真实
// JSONL 文件喂给 replay_session_messages,验证它确实展开成可见的 tool_call /
// tool_result 行(即便老 session 没 metadata,也至少有 N>0 个 tool_call /
// tool_result 出现 —— 那么 resume 后的视图就不会"空"了)。
//
// 这条测试在 CI 中没有那个具体文件时静默跳过,以免 PR 阻塞。

#include <gtest/gtest.h>

#include "session/session_serializer.hpp"
#include "session/session_replay.hpp"
#include "tool/tool_executor.hpp"
#include "provider/llm_provider.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

TEST(SessionReplayRealJsonl, UserSessionDbcoding5_2026_04_26) {
    // 用户报告 "resume 之后工具调用记录空的" 的真实文件。
    fs::path p = "N:/Users/shao/.acecode/projects/a2f69b2969c03220/20260426-062215-0a73-12892.jsonl";
    if (!fs::exists(p)) {
        GTEST_SKIP() << "Real-jsonl smoke test skipped: " << p << " not present";
    }

    std::ifstream in(p);
    ASSERT_TRUE(in.good());

    std::vector<acecode::ChatMessage> messages;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        messages.push_back(acecode::deserialize_message(line));
    }

    EXPECT_GT(messages.size(), 0u) << "JSONL parsed zero messages";

    acecode::ToolExecutor tools;
    auto rows = acecode::replay_session_messages(messages, tools);

    int tool_call_rows = 0, tool_result_rows = 0, assistant_rows = 0, user_rows = 0;
    for (const auto& r : rows) {
        if      (r.role == "tool_call")   ++tool_call_rows;
        else if (r.role == "tool_result") ++tool_result_rows;
        else if (r.role == "assistant")   ++assistant_rows;
        else if (r.role == "user")        ++user_rows;
    }

    // 关键断言:用户的 session 真实有 4 条 assistant+tool_calls 和 10 条 tool。
    // replay 之后 chat 视图应该看到 ≥1 个 tool_call 行 + ≥1 个 tool_result 行。
    // 任何一项为 0 都说明展开器漏了类型,resume 后会出现"工具调用空白"症状。
    EXPECT_GT(tool_call_rows, 0)
        << "tool_call rows missing — resume 后 assistant+tool_calls 行不可见";
    EXPECT_GT(tool_result_rows, 0)
        << "tool_result rows missing — resume 后 role=tool 整条消失";
    EXPECT_GT(assistant_rows + tool_call_rows + tool_result_rows + user_rows, 0)
        << "全部消息消失,replay_session_messages 严重 bug";

    // 老 jsonl 没 metadata,所以 summary/hunks 都应该为空 —— 走 fold 降级是预期。
    int rows_with_summary = 0, rows_with_hunks = 0;
    for (const auto& r : rows) {
        if (r.summary.has_value()) ++rows_with_summary;
        if (r.hunks.has_value())   ++rows_with_hunks;
    }
    EXPECT_EQ(rows_with_summary, 0) << "老 session 不应该出现 summary 字段";
    EXPECT_EQ(rows_with_hunks, 0)   << "老 session 不应该出现 hunks 字段";
}
