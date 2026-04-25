#include "task_complete_tool.hpp"
#include "tool_icons.hpp"

#include <nlohmann/json.hpp>

namespace acecode {

namespace {

// 工具 description。措辞强调"仅在任务全部完成时调用" —— 避免模型把它当成
// "遇到疑问就调一下" 的快捷键。同时明确 summary 必填且非空。
constexpr const char* kToolDescription =
    "Signal that the user's full request has been completed. Call this tool — "
    "and ONLY this tool — when the task is truly done; the agent loop will "
    "return control to the user immediately after this tool returns.\n"
    "\n"
    "Do NOT call this tool to:\n"
    "- Check if the user wants you to continue (just keep working)\n"
    "- Pause for confirmation mid-task (keep working; if you genuinely need "
    "input use AskUserQuestion instead)\n"
    "- Ask the user to verify your work (the user can verify the visible "
    "tool results on their own)\n"
    "\n"
    "The `summary` parameter must be a non-empty string describing what you "
    "accomplished, suitable for the user to read at a glance.";

static ToolResult execute_task_complete(const std::string& arguments_json,
                                        const ToolContext& /*ctx*/) {
    std::string summary;
    try {
        auto j = nlohmann::json::parse(arguments_json);
        if (j.is_object() && j.contains("summary") && j["summary"].is_string()) {
            summary = j["summary"].get<std::string>();
        }
    } catch (const std::exception&) {
        // JSON 解析失败时 summary 保持为空,下一步会走"缺参"分支。
    }

    // Trim 前后空白以便把 "   " / "\n" 这类伪"非空"视作无效。
    auto is_space = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
    };
    std::size_t start = 0;
    while (start < summary.size() && is_space(summary[start])) ++start;
    std::size_t end = summary.size();
    while (end > start && is_space(summary[end - 1])) --end;
    const bool nonempty = (end > start);

    if (!nonempty) {
        return ToolResult{
            "task_complete requires a non-empty summary describing what was accomplished",
            false};
    }

    ToolSummary sum;
    sum.verb = "complete";
    sum.object = "task";
    sum.icon = tool_icon("task_complete");
    sum.metrics.emplace_back("summary", summary);

    ToolResult r;
    r.output = summary;
    r.success = true;
    r.summary = std::move(sum);
    return r;
}

} // namespace

ToolImpl create_task_complete_tool() {
    ToolDef def;
    def.name = "task_complete";
    def.description = kToolDescription;
    def.parameters = {
        {"type", "object"},
        {"required", nlohmann::json::array({"summary"})},
        {"properties", {
            {"summary", {
                {"type", "string"},
                {"minLength", 1},
                {"description",
                 "Short description of what the agent accomplished in this task. "
                 "Non-empty; shown to the user as the completion message."}
            }}
        }}
    };

    ToolImpl impl;
    impl.definition = def;
    impl.execute = execute_task_complete;
    // is_read_only=true 让 PermissionManager::should_auto_allow 在非 Yolo 模式下
    // 也直接放行(与 ask_user_question_tool 同样策略)。task 2.4 另外在
    // permissions.hpp 里明确写出按名放行兜底,避免依赖这个标志。
    impl.is_read_only = true;
    impl.source = ToolSource::Builtin;
    return impl;
}

} // namespace acecode
