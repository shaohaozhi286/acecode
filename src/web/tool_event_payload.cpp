#include "tool_event_payload.hpp"
#include "../session/output_attachments.hpp"
#include "../session/tool_metadata_codec.hpp"

namespace acecode::web {

nlohmann::json tool_summary_to_json(const ToolSummary& s) {
    nlohmann::json out;
    out["icon"]   = s.icon;
    out["verb"]   = s.verb;
    out["object"] = s.object;
    nlohmann::json metrics = nlohmann::json::array();
    for (const auto& [k, v] : s.metrics) {
        metrics.push_back({{"label", k}, {"value", v}});
    }
    out["metrics"] = std::move(metrics);
    return out;
}

nlohmann::json build_tool_start_payload(
    const std::string& tool_name,
    const nlohmann::json& args_payload,
    const std::string& command_preview,
    const std::string& display_override,
    bool is_task_complete,
    const std::string& tool_call_id,
    int tool_index) {
    nlohmann::json p;
    p["tool"]              = tool_name;
    p["args"]              = args_payload;
    p["command_preview"]   = command_preview;
    p["display_override"]  = display_override;
    p["is_task_complete"]  = is_task_complete;
    if (!tool_call_id.empty()) p["tool_call_id"] = tool_call_id;
    if (tool_index >= 0) p["tool_index"] = tool_index;
    return p;
}

nlohmann::json build_tool_update_payload(
    const std::string& tool_name,
    const std::vector<std::string>& tail_lines,
    const std::string& current_partial,
    int total_lines,
    std::size_t total_bytes,
    double elapsed_seconds,
    const std::string& tool_call_id,
    int tool_index) {
    nlohmann::json p;
    p["tool"]            = tool_name;
    p["tail_lines"]      = tail_lines;
    p["current_partial"] = current_partial;
    p["total_lines"]     = total_lines;
    p["total_bytes"]     = total_bytes;
    p["elapsed_seconds"] = elapsed_seconds;
    if (!tool_call_id.empty()) p["tool_call_id"] = tool_call_id;
    if (tool_index >= 0) p["tool_index"] = tool_index;
    return p;
}

nlohmann::json build_tool_end_payload(
    const std::string& tool_name,
    const ToolResult& result,
    double elapsed_seconds,
    const std::string& output_snippet,
    const std::string& tool_call_id,
    int tool_index) {
    nlohmann::json p;
    p["tool"]            = tool_name;
    p["success"]         = result.success;
    p["elapsed_seconds"] = elapsed_seconds;
    if (!tool_call_id.empty()) p["tool_call_id"] = tool_call_id;
    if (tool_index >= 0) p["tool_index"] = tool_index;
    if (result.summary.has_value()) {
        p["summary"] = tool_summary_to_json(*result.summary);
    }
    if (result.metadata.is_object() && !result.metadata.empty()) {
        p["metadata"] = result.metadata;
    }
    if (!result.success && !output_snippet.empty()) {
        p["output"] = output_snippet;
    }
    // hunks 字段总是存在(空数组而不是省略),便于前端写"if (p.hunks.length)"
    // 而不必先 contains 判断。前端 file_edit / file_write 走 diff2html 渲染。
    if (result.hunks.has_value()) {
        p["hunks"] = encode_tool_hunks(*result.hunks);
    } else {
        p["hunks"] = nlohmann::json::array();
    }
    p["attachments"] = result.has_attachments()
        ? output_attachments_from_content_parts(
              output_attachments_to_content_parts(result.attachments))
        : nlohmann::json::array();
    if (!result.attachment_warnings.empty()) {
        p["attachment_warnings"] = result.attachment_warnings;
    }
    return p;
}

} // namespace acecode::web
