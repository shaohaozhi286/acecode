// 覆盖 ToolExecutor::build_tool_call_preview —— 由 agent_loop 在每个 tool_call
// 发出时调用,结果塞进 ChatMessage::display_override。TUI 据此渲染精简的
// "→ tool  preview" 一行,替代原来的 "[Tool: X] {JSON}"。
// 这里用纯函数测试(不起 AgentLoop),既足以验证新逻辑又不用 mock 整个 provider 链。

#include <gtest/gtest.h>

#include "tool/tool_executor.hpp"

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
