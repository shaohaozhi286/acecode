// 覆盖 src/tool/task_complete_tool.cpp 的核心行为:
// - 合法非空 summary → success=true,output 原样回显,ToolSummary 填充
// - 缺参 / 空串 / 纯空白 → success=false + 明确错误提示
// - JSON 解析失败 → 走缺参分支,不抛异常
// - schema 声明 summary 必填 + minLength>=1
// - 工具默认 is_read_only=true,保证 PermissionManager 放行
//
// 纯函数级测试,不依赖 TuiState / FTXUI / 线程,放 tests/tool/ 目录统一 glob。

#include <gtest/gtest.h>

#include "tool/task_complete_tool.hpp"
#include "tool/tool_executor.hpp"

#include <nlohmann/json.hpp>

using acecode::create_task_complete_tool;
using acecode::ToolContext;
using acecode::ToolImpl;
using acecode::ToolResult;

namespace {

// 小辅助:直接跑 tool,不带 context(task_complete 不使用 stream / abort)
ToolResult run(const std::string& args_json) {
    ToolImpl impl = create_task_complete_tool();
    ToolContext ctx;
    return impl.execute(args_json, ctx);
}

bool has_metric(const acecode::ToolSummary& s, const std::string& k,
                std::string* out_value = nullptr) {
    for (const auto& kv : s.metrics) {
        if (kv.first == k) {
            if (out_value) *out_value = kv.second;
            return true;
        }
    }
    return false;
}

} // namespace

// 场景:合法 summary → success + ToolSummary 填充 + output 回显
TEST(TaskCompleteTool, ValidSummarySucceeds) {
    nlohmann::json args = {{"summary", "Refactored auth module and added 4 tests"}};
    ToolResult r = run(args.dump());

    EXPECT_TRUE(r.success);
    EXPECT_EQ(r.output, "Refactored auth module and added 4 tests");
    ASSERT_TRUE(r.summary.has_value());
    EXPECT_EQ(r.summary->verb, "complete");
    EXPECT_EQ(r.summary->object, "task");
    EXPECT_FALSE(r.summary->icon.empty());
    std::string stored_summary;
    EXPECT_TRUE(has_metric(*r.summary, "summary", &stored_summary));
    EXPECT_EQ(stored_summary, "Refactored auth module and added 4 tests");
}

// 场景:缺 summary 字段 → success=false + 明确错误信息
TEST(TaskCompleteTool, MissingSummaryFails) {
    nlohmann::json args = nlohmann::json::object();  // 完全没有 summary 字段
    ToolResult r = run(args.dump());

    EXPECT_FALSE(r.success);
    EXPECT_NE(r.output.find("non-empty summary"), std::string::npos);
    EXPECT_FALSE(r.summary.has_value());
}

// 场景:空字符串 summary → 同样视作缺参
TEST(TaskCompleteTool, EmptySummaryFails) {
    nlohmann::json args = {{"summary", ""}};
    ToolResult r = run(args.dump());
    EXPECT_FALSE(r.success);
    EXPECT_NE(r.output.find("non-empty summary"), std::string::npos);
}

// 场景:只有空白字符(空格 / 换行 / tab)→ 也判定为空,防止模型拿"   "糊弄
TEST(TaskCompleteTool, WhitespaceOnlySummaryFails) {
    nlohmann::json args = {{"summary", "   \n\t  "}};
    ToolResult r = run(args.dump());
    EXPECT_FALSE(r.success);
    EXPECT_NE(r.output.find("non-empty summary"), std::string::npos);
}

// 场景:summary 字段类型错误(给了数字)→ 按缺参处理,不崩
TEST(TaskCompleteTool, WrongTypeSummaryFails) {
    nlohmann::json args = {{"summary", 42}};
    ToolResult r = run(args.dump());
    EXPECT_FALSE(r.success);
}

// 场景:完全无效的 JSON → catch 后走缺参分支,绝不抛异常
TEST(TaskCompleteTool, InvalidJsonFailsGracefully) {
    ToolResult r = run("not a json {{{{");
    EXPECT_FALSE(r.success);
    EXPECT_NE(r.output.find("non-empty summary"), std::string::npos);
}

// 场景:summary 前后带空白但中间有内容 → 接受(模型输出经常带换行)
TEST(TaskCompleteTool, SummaryWithSurroundingWhitespaceSucceeds) {
    nlohmann::json args = {{"summary", "  Done.  \n"}};
    ToolResult r = run(args.dump());
    EXPECT_TRUE(r.success);
    // output 保留原始文本(包括空白)— 下游 TUI 自己处理视觉去除
    EXPECT_EQ(r.output, "  Done.  \n");
}

// 场景:ToolImpl 默认 is_read_only=true —— 让 PermissionManager 不弹确认
TEST(TaskCompleteTool, IsReadOnly) {
    ToolImpl impl = create_task_complete_tool();
    EXPECT_TRUE(impl.is_read_only);
    EXPECT_EQ(impl.source, acecode::ToolSource::Builtin);
}

// 场景:schema 声明正确 —— summary 必填 + minLength>=1 + 工具名
TEST(TaskCompleteTool, SchemaShape) {
    ToolImpl impl = create_task_complete_tool();
    EXPECT_EQ(impl.definition.name, "task_complete");
    EXPECT_FALSE(impl.definition.description.empty());

    const auto& params = impl.definition.parameters;
    ASSERT_TRUE(params.contains("required"));
    ASSERT_TRUE(params["required"].is_array());
    bool has_required_summary = false;
    for (const auto& v : params["required"]) {
        if (v.is_string() && v.get<std::string>() == "summary") {
            has_required_summary = true;
            break;
        }
    }
    EXPECT_TRUE(has_required_summary);

    ASSERT_TRUE(params.contains("properties"));
    ASSERT_TRUE(params["properties"].contains("summary"));
    const auto& summary_schema = params["properties"]["summary"];
    EXPECT_EQ(summary_schema["type"], "string");
    EXPECT_TRUE(summary_schema.contains("minLength"));
    EXPECT_GE(summary_schema["minLength"].get<int>(), 1);
}
