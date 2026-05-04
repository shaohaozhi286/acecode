// 覆盖 src/web/tool_event_payload.cpp。这套 helper 把 ToolResult/ToolSummary
// 序列化成 daemon → 浏览器的 tool_start/tool_update/tool_end WS payload。
// 一旦回归:
//   - summary 字段错位 → 前端 ToolSummary 行渲染崩
//   - is_task_complete 错配 → task_complete 工具不显示 "Done:" 紧凑行
//   - failure 不带 output 字段 → 前端拿不到 stderr 摘要
// 因此每个 TEST 锁一条契约。

#include <gtest/gtest.h>

#include "web/tool_event_payload.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using acecode::ToolResult;
using acecode::ToolSummary;
using acecode::web::build_tool_end_payload;
using acecode::web::build_tool_start_payload;
using acecode::web::build_tool_update_payload;
using acecode::web::tool_summary_to_json;

// 场景: ToolSummary → JSON 字段齐全且 metrics 保持顺序(前端按顺序展示)。
TEST(ToolEventPayload, SummaryFieldsAndMetricOrdering) {
    ToolSummary s;
    s.icon   = "→";
    s.verb   = "Ran";
    s.object = "ls -la";
    s.metrics = {{"lines", "42"}, {"bytes", "1024"}};

    auto j = tool_summary_to_json(s);
    EXPECT_EQ(j["icon"], "→");
    EXPECT_EQ(j["verb"], "Ran");
    EXPECT_EQ(j["object"], "ls -la");
    ASSERT_TRUE(j["metrics"].is_array());
    ASSERT_EQ(j["metrics"].size(), 2u);
    // 顺序断言: lines 在 bytes 之前
    EXPECT_EQ(j["metrics"][0]["label"], "lines");
    EXPECT_EQ(j["metrics"][0]["value"], "42");
    EXPECT_EQ(j["metrics"][1]["label"], "bytes");
    EXPECT_EQ(j["metrics"][1]["value"], "1024");
}

// 场景: 普通 tool_start payload 含全部 5 个字段(tool/args/command_preview/
// display_override/is_task_complete);is_task_complete 默认 false。
TEST(ToolEventPayload, ToolStartFullPayload) {
    nlohmann::json args = {{"command", "echo hi"}};
    auto p = build_tool_start_payload("bash", args, "echo hi", "bash  echo hi", false);
    EXPECT_EQ(p["tool"], "bash");
    EXPECT_EQ(p["args"]["command"], "echo hi");
    EXPECT_EQ(p["command_preview"], "echo hi");
    EXPECT_EQ(p["display_override"], "bash  echo hi");
    EXPECT_EQ(p["is_task_complete"], false);
}

// 场景: task_complete 工具的 tool_start 必须带 is_task_complete=true,
// 让前端走紧凑 "Done:" 行而不是常规工具块。
TEST(ToolEventPayload, ToolStartTaskCompleteFlag) {
    nlohmann::json args = {{"summary", "已完成 X"}};
    auto p = build_tool_start_payload("task_complete", args, "task_complete",
                                        "", true);
    EXPECT_EQ(p["tool"], "task_complete");
    EXPECT_EQ(p["is_task_complete"], true);
    EXPECT_EQ(p["args"]["summary"], "已完成 X");
}

// 场景: tool_update 含 5-line tail + current_partial + 状态 3 件套
// (total_lines/total_bytes/elapsed_seconds)。
TEST(ToolEventPayload, ToolUpdateFiveLineTail) {
    std::vector<std::string> tail = {"line A", "line B", "line C", "line D", "line E"};
    auto p = build_tool_update_payload("bash", tail, "partial...",
                                         100, 4096, 12.5);
    EXPECT_EQ(p["tool"], "bash");
    ASSERT_TRUE(p["tail_lines"].is_array());
    EXPECT_EQ(p["tail_lines"].size(), 5u);
    EXPECT_EQ(p["tail_lines"][4], "line E");
    EXPECT_EQ(p["current_partial"], "partial...");
    EXPECT_EQ(p["total_lines"], 100);
    EXPECT_EQ(p["total_bytes"], 4096);
    EXPECT_DOUBLE_EQ(p["elapsed_seconds"].get<double>(), 12.5);
}

// 场景: tool_update 空 tail 也合法(工具刚启动还没输出)。
TEST(ToolEventPayload, ToolUpdateEmptyTail) {
    auto p = build_tool_update_payload("bash", {}, "", 0, 0, 0.0);
    ASSERT_TRUE(p["tail_lines"].is_array());
    EXPECT_EQ(p["tail_lines"].size(), 0u);
    EXPECT_EQ(p["current_partial"], "");
    EXPECT_EQ(p["total_lines"], 0);
}

// 场景: 普通成功的 tool_end:success=true,带 summary,不带 output(前端不需要)。
TEST(ToolEventPayload, ToolEndSuccessWithSummary) {
    ToolResult r;
    r.success = true;
    r.output  = "ok\n";
    ToolSummary s; s.icon = "✓"; s.verb = "Read"; s.object = "foo.txt";
    s.metrics = {{"lines", "12"}};
    r.summary = s;

    auto p = build_tool_end_payload("file_read", r, 0.05, "");
    EXPECT_EQ(p["tool"], "file_read");
    EXPECT_EQ(p["success"], true);
    EXPECT_DOUBLE_EQ(p["elapsed_seconds"].get<double>(), 0.05);
    ASSERT_TRUE(p.contains("summary"));
    EXPECT_EQ(p["summary"]["verb"], "Read");
    // success=true 时不应出现 output 字段
    EXPECT_FALSE(p.contains("output"));
}

// 场景: 失败 + output_snippet 非空时,output 字段附在 payload 上(前端 dim 显示前 3 行)。
TEST(ToolEventPayload, ToolEndFailureCarriesOutput) {
    ToolResult r;
    r.success = false;
    r.output  = "error: cannot open file\nat line 3\n";
    auto p = build_tool_end_payload("file_read", r, 0.01,
                                       "error: cannot open file\nat line 3\n");
    EXPECT_EQ(p["success"], false);
    ASSERT_TRUE(p.contains("output"));
    EXPECT_NE(p["output"].get<std::string>().find("error:"), std::string::npos);
    // 没有 summary 字段(本失败工具未提供)
    EXPECT_FALSE(p.contains("summary"));
}

// 场景: 失败但 output_snippet 为空(caller 没截 — 比如工具直接抛错没 stderr),
// 不能写一个空的 output 字段(前端会渲染空 dim 行)。
TEST(ToolEventPayload, ToolEndFailureEmptySnippetOmitsField) {
    ToolResult r;
    r.success = false;
    r.output  = "";
    auto p = build_tool_end_payload("bash", r, 0.0, "");
    EXPECT_EQ(p["success"], false);
    EXPECT_FALSE(p.contains("output"));
}

// 场景: ToolResult 没 summary(工具未 opt-in)+ 成功 → payload 缺 summary 字段,
// 前端按"传统模式"fallback 渲染。这是 streamline-tool-output-display 的兼容 path。
TEST(ToolEventPayload, ToolEndSuccessWithoutSummaryFallback) {
    ToolResult r;
    r.success = true;
    r.output  = "ok";
    auto p = build_tool_end_payload("grep", r, 0.0, "");
    EXPECT_EQ(p["success"], true);
    EXPECT_FALSE(p.contains("summary"));
}

// 场景: ToolResult.hunks 无值 → payload.hunks 总是空数组(不省略也不 null)。
// 前端契约: 永远是 array,前端写 if (hunks.length) 不必先 contains 检查。
TEST(ToolEventPayload, ToolEndAlwaysIncludesHunksField) {
    ToolResult r;
    r.success = true;
    auto p = build_tool_end_payload("bash", r, 0.0, "");
    ASSERT_TRUE(p.contains("hunks"));
    EXPECT_TRUE(p["hunks"].is_array());
    EXPECT_EQ(p["hunks"].size(), 0u);
}

// 场景: ToolResult.hunks 有值 → 通过 encode_tool_hunks 序列化进 payload.hunks。
// file_edit 的 diff 着色靠这个字段;一旦丢字段,Web 上 file_edit 退化到纯文本。
TEST(ToolEventPayload, ToolEndCarriesHunksFromResult) {
    ToolResult r;
    r.success = true;

    acecode::DiffHunk h;
    h.old_start = 1; h.old_count = 1; h.new_start = 1; h.new_count = 1;
    acecode::DiffLine rm;
    rm.kind = acecode::DiffLineKind::Removed;
    rm.text = "old line";
    acecode::DiffLine ad;
    ad.kind = acecode::DiffLineKind::Added;
    ad.text = "new line";
    h.lines = {rm, ad};
    r.hunks = std::vector<acecode::DiffHunk>{h};

    auto p = build_tool_end_payload("file_edit", r, 0.05, "");
    ASSERT_TRUE(p["hunks"].is_array());
    ASSERT_EQ(p["hunks"].size(), 1u);
    EXPECT_EQ(p["hunks"][0]["old_start"], 1);
    ASSERT_TRUE(p["hunks"][0]["lines"].is_array());
    EXPECT_EQ(p["hunks"][0]["lines"].size(), 2u);
    EXPECT_EQ(p["hunks"][0]["lines"][0]["kind"], "removed");
    EXPECT_EQ(p["hunks"][0]["lines"][1]["kind"], "added");
}
