#pragma once

// 把 agent_loop.cpp 里 emit ToolStart/ToolUpdate/ToolEnd 用到的"将 C++
// 结构体序列化成前端契约 JSON"提取出来。原因:agent_loop.cpp 内联 JSON
// 构造不易单测;抽成 helper 后既可在 agent_loop.cpp 复用,又可以在
// tests/web/tool_event_payload_test.cpp 直接驱动验证字段齐全。
//
// 这套 payload 的 schema 见 openspec/changes/add-web-chat-ui/specs/web-daemon/spec.md
// "WebSocket 双向消息协议" Requirement 的 MODIFIED 节(tool_start/tool_update/
// tool_end 字段表)。

#include "../tool/tool_executor.hpp"

#include <nlohmann/json.hpp>

#include <deque>
#include <string>
#include <vector>

namespace acecode::web {

// 把 ToolSummary 转成前端约定的 JSON object:
//   { icon, verb, object, metrics: [{label, value}, ...] }
// metrics 用 vector-of-object(不是 map),因为顺序对前端展示有意义。
nlohmann::json tool_summary_to_json(const ToolSummary& s);

// 构造 tool_start payload。`display_override` 和 `is_task_complete` 由 caller
// 传入(caller 已经知道怎么算)。`args_payload` 是已 parse 的 args(失败时
// 透字符串)— caller 负责处理 parse 失败 fallback,本函数只拼字段。
nlohmann::json build_tool_start_payload(
    const std::string& tool_name,
    const nlohmann::json& args_payload,
    const std::string& command_preview,
    const std::string& display_override,
    bool is_task_complete);

// 构造 tool_update payload。tail_lines 最多 5 条由 caller 截好(本函数不截)。
nlohmann::json build_tool_update_payload(
    const std::string& tool_name,
    const std::vector<std::string>& tail_lines,
    const std::string& current_partial,
    int total_lines,
    std::size_t total_bytes,
    double elapsed_seconds);

// 构造 tool_end payload。result.summary 缺省时 "summary" 字段不出现;
// success=false 时附带前 N 行 stderr/output 给前端 dim 显示(N 由 caller
// 已经截好,本函数不截)。
nlohmann::json build_tool_end_payload(
    const std::string& tool_name,
    const ToolResult& result,
    double elapsed_seconds,
    const std::string& output_snippet);

} // namespace acecode::web
