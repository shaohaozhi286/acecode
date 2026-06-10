// 覆盖 ToolExecutor::build_tool_call_preview —— 由 agent_loop 在每个 tool_call
// 发出时调用,结果塞进 ChatMessage::display_override。TUI 据此渲染精简的
// "→ tool  preview" 一行,替代原来的 "[Tool: X] {JSON}"。
// 这里用纯函数测试(不起 AgentLoop),既足以验证新逻辑又不用 mock 整个 provider 链。

#include <gtest/gtest.h>

#include "tool/tool_executor.hpp"
#include "utils/encoding.hpp"

#include <nlohmann/json.hpp>

using acecode::ToolExecutor;

// 场景 1: bash 工具,command 在 60 字符以内 → 整条命令原样显示。
TEST(ToolCallPreview, BashShortCommand) {
    nlohmann::json args = {{"command", "npm install"}};
    auto preview = ToolExecutor::build_tool_call_preview("bash", args.dump());
    EXPECT_EQ(preview, "bash  npm install");
}

// 场景 2: bash 工具,command 超过 60 字符 → 截断到 57 + "..."。
TEST(ToolCallPreview, BashLongCommandTruncated) {
    std::string long_cmd(100, 'x'); // 100 个 'x'
    nlohmann::json args = {{"command", long_cmd}};
    auto preview = ToolExecutor::build_tool_call_preview("bash", args.dump());
    // "bash  " + 57 x + "..."
    EXPECT_EQ(preview.substr(0, 6), "bash  ");
    EXPECT_NE(preview.find("..."), std::string::npos);
    EXPECT_LE(preview.size(), static_cast<size_t>(6 + 60));
}

// 场景 2b: bash 工具,超长 UTF-8 命令包含中文时,截断必须落在码点边界上,
// 否则 display_override 会变成非法 UTF-8,后续写 JSONL 时触发 nlohmann::json
// 的 invalid UTF-8 异常。
TEST(ToolCallPreview, BashLongUtf8CommandTruncatedOnCodepointBoundary) {
    std::string long_cmd;
    for (int i = 0; i < 21; ++i) long_cmd += "中"; // 63 bytes

    nlohmann::json args = {{"command", long_cmd}};
    auto preview = ToolExecutor::build_tool_call_preview("bash", args.dump());

    std::string expected_cmd;
    for (int i = 0; i < 19; ++i) expected_cmd += "中"; // 57 bytes
    EXPECT_EQ(preview, "bash  " + expected_cmd + "...");
    EXPECT_TRUE(acecode::is_valid_utf8(preview));
}

// 场景 3: file_read 工具,短路径 → 原样显示。
TEST(ToolCallPreview, FileReadShortPath) {
    nlohmann::json args = {{"file_path", "src/main.cpp"}};
    auto preview = ToolExecutor::build_tool_call_preview("file_read", args.dump());
    EXPECT_EQ(preview, "file_read  src/main.cpp");
}

// 场景 4: file_edit 工具,超长路径(> 40 字符)应做末尾保留式截断,
// 让文件名保持可见。
TEST(ToolCallPreview, FileEditLongPathTailKept) {
    std::string long_path = std::string(60, 'a') + "/final_filename.cpp"; // 79 字符
    nlohmann::json args = {
        {"file_path", long_path},
        {"old_string", "x"},
        {"new_string", "y"}
    };
    auto preview = ToolExecutor::build_tool_call_preview("file_edit", args.dump());
    // "file_edit  " + 40 字符截尾路径(含文件名)
    EXPECT_EQ(preview.substr(0, 11), "file_edit  ");
    EXPECT_NE(preview.find("final_filename.cpp"), std::string::npos);
    EXPECT_NE(preview.find("..."), std::string::npos);
}

// 场景 4b: file_* 工具的路径尾部保留式截断遇到中文目录名时,同样必须保持
// UTF-8 有效,否则 tool_call 预览或摘要字段会在落盘时崩溃。
TEST(ToolCallPreview, FileEditLongUtf8PathRemainsValidUtf8) {
    std::string long_path;
    for (int i = 0; i < 20; ++i) long_path += "目录";
    long_path += "/最终文件.cpp";

    nlohmann::json args = {
        {"file_path", long_path},
        {"old_string", "x"},
        {"new_string", "y"}
    };
    auto preview = ToolExecutor::build_tool_call_preview("file_edit", args.dump());

    EXPECT_EQ(preview.substr(0, 11), "file_edit  ");
    EXPECT_NE(preview.find("最终文件.cpp"), std::string::npos);
    EXPECT_NE(preview.find("..."), std::string::npos);
    EXPECT_TRUE(acecode::is_valid_utf8(preview));
}

// 场景 4c: skill_view 工具只显示 skill 名和可选支持文件路径,避免 TUI
// tool_call 行渲染完整 JSON 参数。
TEST(ToolCallPreview, SkillViewShowsSkillName) {
    nlohmann::json args = {{"name", "openspec-explore"}};
    auto preview = ToolExecutor::build_tool_call_preview("skill_view", args.dump());
    EXPECT_EQ(preview, "skill_view  openspec-explore");
}

TEST(ToolCallPreview, SkillViewShowsSupportingFilePath) {
    nlohmann::json args = {
        {"name", "openspec-explore"},
        {"file_path", "references/example.md"}
    };
    auto preview = ToolExecutor::build_tool_call_preview("skill_view", args.dump());
    EXPECT_EQ(preview, "skill_view  openspec-explore references/example.md");
}

// 场景 4d: AskUserQuestion 工具调用预览显示问题数量与首题摘要,避免
// TUI/desktop 直接露出完整 JSON 参数。
TEST(ToolCallPreview, AskUserQuestionShowsReadableQuestionSummary) {
    nlohmann::json args = {
        {"questions", nlohmann::json::array({
            {
                {"question", "希望我直接修改还是先给出方案让你确认?"},
                {"header", "方式"},
                {"options", nlohmann::json::array({
                    {{"label", "直接修改"}, {"description", ""}},
                    {{"label", "先给方案"}, {"description", ""}}
                })}
            },
            {
                {"question", "是否需要兼容旧版 API?"},
                {"header", "兼容"},
                {"options", nlohmann::json::array({
                    {{"label", "需要"}, {"description", ""}},
                    {{"label", "不需要"}, {"description", ""}}
                })}
            }
        })}
    };
    auto preview = ToolExecutor::build_tool_call_preview("AskUserQuestion", args.dump());
    EXPECT_NE(preview.find("AskUserQuestion"), std::string::npos);
    EXPECT_NE(preview.find("2"), std::string::npos);
    EXPECT_NE(preview.find("希望我直接修改"), std::string::npos);
    EXPECT_TRUE(acecode::is_valid_utf8(preview));
}

// 场景 5: 未知工具 → 返回空字符串,TUI fallback 到 legacy 渲染。
TEST(ToolCallPreview, UnknownToolReturnsEmpty) {
    nlohmann::json args = {{"any_field", "value"}};
    auto preview = ToolExecutor::build_tool_call_preview("some_mcp_tool", args.dump());
    EXPECT_TRUE(preview.empty());
}

// 场景 6: 参数 JSON 解析失败 → 返回空,不崩溃。
TEST(ToolCallPreview, MalformedJsonReturnsEmpty) {
    auto preview = ToolExecutor::build_tool_call_preview("bash", "not json at all");
    EXPECT_TRUE(preview.empty());
}
