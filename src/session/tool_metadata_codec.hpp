#pragma once

// Tool 视觉字段(ToolSummary、vector<DiffHunk>)与 nlohmann::json 的双向编解码。
//
// 用途:为 resume 视觉还原服务,被两端使用:
//   - 写盘端(agent_loop.cpp):工具执行完成后,把 ToolResult.summary 与
//     ToolResult.hunks 编码后塞进 ChatMessage.metadata 的 "tool_summary" /
//     "tool_hunks" 子键,再通过 session_serializer 落盘到 JSONL。
//   - 读盘端(session_replay.cpp):resume 时把磁盘上的 metadata 解码回
//     ToolSummary / vector<DiffHunk>,回填 TuiState::Message,使彩色 diff
//     与绿/红摘要在 resume 后一比一还原。
//
// 顶层 ChatMessage 的 metadata 字段已经在 session_serializer 的白名单里;
// 本 codec 只是约定它下面两个 JSON 子键的形态,顶层 schema 不变。
//
// 设计原则:
//   - encode 路径无歧义、稳定,空 metrics → 输出空数组而不是省略字段。
//   - decode 路径"宽进严出":类型错 / 必需字段缺失 / 未知 line.kind 一律
//     返回 std::nullopt 而**不抛异常**,这样老 session 或人工手改的 JSONL
//     不会让整次 resume 崩溃 —— 只是该字段降级到空,渲染走 fold 路径。

#include "../tool/diff_utils.hpp"
#include "../tool/tool_executor.hpp"

#include <nlohmann/json.hpp>
#include <optional>
#include <vector>

namespace acecode {

// 把 ToolSummary 编码为 JSON 对象。空 metrics 输出空数组(而不是省略字段),
// 保证 decode 拿到的对象形态稳定可预测。
nlohmann::json encode_tool_summary(const ToolSummary& s);

// 从 JSON 对象解码 ToolSummary。verb / object / icon 必须是字符串,
// metrics 必须是 [[string, string], ...] 形式的数组。任何字段类型错 →
// 返回 std::nullopt,**不抛异常**。允许字段缺失(取默认空值)。
std::optional<ToolSummary> decode_tool_summary(const nlohmann::json& j);

// 把 vector<DiffHunk> 编码为 JSON 数组。每个 hunk:
//   {old_start, old_count, new_start, new_count, lines: [...]}
// 每个 line:
//   {kind: "context"|"added"|"removed", text, old_line_no?, new_line_no?}
// optional<int> 为空时不输出该字段(用 contains 判断),避免 JSON null
// 与"未设置"的歧义。
nlohmann::json encode_tool_hunks(const std::vector<DiffHunk>& h);

// 从 JSON 数组解码 vector<DiffHunk>。任何字段类型错 / 必需字段缺失 /
// 未知 line.kind 都返回 std::nullopt。**不抛异常**。
// 未识别的额外字段(如未来版本扩展)被忽略,这是向前兼容的保留口子。
std::optional<std::vector<DiffHunk>> decode_tool_hunks(const nlohmann::json& j);

} // namespace acecode
