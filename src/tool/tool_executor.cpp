#include "tool_executor.hpp"
#include "../session/output_attachments.hpp"
#include "utils/logger.hpp"
#include "utils/encoding.hpp"
#include "utils/utf8_path.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>

namespace acecode {

namespace {

std::string normalized_path_for_scope(const std::string& raw,
                                      const std::string& cwd) {
    if (raw.empty()) return {};

    namespace fs = std::filesystem;
    fs::path path = path_from_utf8(raw);
    if (path.is_relative()) {
        if (cwd.empty()) return {};
        path = path_from_utf8(cwd) / path;
    }

    std::error_code ec;
    fs::path normalized = fs::weakly_canonical(path, ec);
    if (ec) {
        ec.clear();
        normalized = fs::absolute(path, ec);
        if (ec) normalized = path;
    }

    std::string text = path_to_utf8_generic(normalized.lexically_normal());
    if (text.rfind("//?/", 0) == 0) text.erase(0, 4);
    while (text.size() > 1 && text.back() == '/') text.pop_back();
#ifdef _WIN32
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
#endif
    return text;
}

} // namespace

bool ToolContext::is_workspace_scratch_path(const std::string& file_path) const {
    if (file_path.empty() || scratch_dir.empty()) return false;

    const auto scratch_root = path_from_utf8(scratch_dir).parent_path();
    if (scratch_root.empty()) return false;

    const std::string file =
        normalized_path_for_scope(file_path, cwd);
    const std::string root =
        normalized_path_for_scope(path_to_utf8(scratch_root), cwd);
    if (file.empty() || root.empty()) return false;
    return file == root ||
        (file.size() > root.size() &&
         file.compare(0, root.size(), root) == 0 &&
         file[root.size()] == '/');
}

void mark_workspace_scratch_change(ToolResult& result, const ToolContext& ctx) {
    if (!result.success ||
        !result.summary.has_value() ||
        !result.hunks.has_value() ||
        result.hunks->empty()) {
        return;
    }
    if (ctx.is_workspace_scratch_path(result.summary->object)) {
        result.metadata[kExcludeFromTurnChangeSummaryMetadata] = true;
    }
}

void ToolExecutor::register_tool(const ToolImpl& tool) {
    LOG_INFO("Registering tool: " + tool.definition.name);
    std::lock_guard<std::mutex> lk(tools_mu_);
    tools_[tool.definition.name] = tool;
}

bool ToolExecutor::unregister_tool(const std::string& name) {
    std::lock_guard<std::mutex> lk(tools_mu_);
    auto it = tools_.find(name);
    if (it == tools_.end()) return false;
    LOG_INFO("Unregistering tool: " + name);
    tools_.erase(it);
    return true;
}

std::vector<ToolDef> ToolExecutor::get_tool_definitions() const {
    std::vector<ToolDef> defs;
    std::lock_guard<std::mutex> lk(tools_mu_);
    for (const auto& [name, impl] : tools_) {
        defs.push_back(impl.definition);
    }
    return defs;
}

std::vector<ToolDef> ToolExecutor::get_tool_definitions_by_source(ToolSource source) const {
    std::vector<ToolDef> defs;
    std::lock_guard<std::mutex> lk(tools_mu_);
    for (const auto& [name, impl] : tools_) {
        if (impl.source == source) defs.push_back(impl.definition);
    }
    return defs;
}

ToolResult ToolExecutor::execute(const std::string& tool_name, const std::string& arguments_json) const {
    return execute(tool_name, arguments_json, ToolContext{});
}

ToolResult ToolExecutor::execute(const std::string& tool_name, const std::string& arguments_json,
                                 const ToolContext& ctx) const {
    ToolImpl impl;
    {
        std::lock_guard<std::mutex> lk(tools_mu_);
        auto it = tools_.find(tool_name);
        if (it == tools_.end()) {
            LOG_ERROR("execute: unknown tool '" + tool_name + "'");
            return ToolResult{"[Error] Unknown tool: " + tool_name, false};
        }
        impl = it->second;
    }
    ToolContext effective_ctx = ctx;
    effective_ctx.tool_executor = const_cast<ToolExecutor*>(this);
    auto result = impl.execute(arguments_json, effective_ctx);
    return result;
}

bool ToolExecutor::has_tool(const std::string& name) const {
    std::lock_guard<std::mutex> lk(tools_mu_);
    return tools_.find(name) != tools_.end();
}

bool ToolExecutor::is_read_only(const std::string& name) const {
    std::lock_guard<std::mutex> lk(tools_mu_);
    auto it = tools_.find(name);
    return it != tools_.end() && it->second.is_read_only;
}

std::string ToolExecutor::generate_tools_prompt() const {
    std::ostringstream oss;
    std::lock_guard<std::mutex> lk(tools_mu_);
    for (const auto& [name, impl] : tools_) {
        oss << "## " << impl.definition.name << "\n"
            << "Description: " << impl.definition.description << "\n"
            << "Parameters:\n```json\n"
            << impl.definition.parameters.dump(2) << "\n```\n\n";
    }
    return oss.str();
}

ChatMessage ToolExecutor::format_tool_result(const std::string& tool_call_id, const ToolResult& result) {
    ChatMessage msg;
    msg.role = "tool";
    msg.content = ensure_utf8(result.output);
    msg.tool_call_id = tool_call_id;
    if (result.metadata.is_object() && !result.metadata.empty()) {
        msg.metadata = result.metadata;
    }
    if (result.has_attachments()) {
        msg.content_parts = output_attachments_to_content_parts(result.attachments);
    }
    if (!result.attachment_warnings.empty()) {
        msg.metadata["attachment_warnings"] = result.attachment_warnings;
    }
    return msg;
}

std::string ToolExecutor::build_tool_call_preview(const std::string& tool_name,
                                                  const std::string& arguments_json) {
    try {
        auto j = nlohmann::json::parse(arguments_json);
        if (tool_name == "bash") {
            if (j.contains("command") && j["command"].is_string()) {
                std::string cmd = j["command"].get<std::string>();
                cmd = truncate_utf8_prefix(cmd, 60);
                return tool_name + "  " + cmd;
            }
        } else if (tool_name == "file_read" || tool_name == "file_write" ||
                   tool_name == "file_edit") {
            if (j.contains("file_path") && j["file_path"].is_string()) {
                std::string p = j["file_path"].get<std::string>();
                // Tail-truncate long paths so the filename stays visible.
                p = truncate_utf8_suffix(p, 40);
                return tool_name + "  " + p;
            }
        } else if (tool_name == "grep" || tool_name == "glob") {
            if (j.contains("pattern") && j["pattern"].is_string()) {
                std::string preview = j["pattern"].get<std::string>();
                if (j.contains("path") && j["path"].is_string() &&
                    !j["path"].get<std::string>().empty()) {
                    preview += " " + j["path"].get<std::string>();
                }
                preview = truncate_utf8_prefix(preview, 60);
                return tool_name + "  " + preview;
            }
        } else if (tool_name == "web_search") {
            if (j.contains("query") && j["query"].is_string()) {
                std::string q = truncate_utf8_prefix(
                    j["query"].get<std::string>(), 60);
                return tool_name + "  " + q;
            }
        } else if (tool_name == "skill_view") {
            if (j.contains("name") && j["name"].is_string()) {
                std::string preview = j["name"].get<std::string>();
                if (j.contains("file_path") && j["file_path"].is_string()) {
                    preview += " ";
                    preview += j["file_path"].get<std::string>();
                }
                preview = truncate_utf8_prefix(preview, 80);
                return tool_name + "  " + preview;
            }
        } else if (tool_name == "spawn_subagent") {
            if (j.contains("prompt") && j["prompt"].is_string()) {
                std::string prompt = truncate_utf8_prefix(j["prompt"].get<std::string>(), 60);
                return std::string("启动子代理  ") + prompt;
            }
            return "启动子代理";
        } else if (tool_name == "wait_subagent") {
            return "等待子代理";
        } else if (tool_name == "AskUserQuestion") {
            if (j.contains("questions") && j["questions"].is_array()) {
                const auto& questions = j["questions"];
                std::string preview = "询问 " + std::to_string(questions.size()) + " 个确认项";
                if (!questions.empty() && questions[0].is_object() &&
                    questions[0].contains("question") &&
                    questions[0]["question"].is_string()) {
                    std::string first = questions[0]["question"].get<std::string>();
                    first = truncate_utf8_prefix(first, 50);
                    if (!first.empty()) {
                        preview += ": " + first;
                    }
                }
                return tool_name + "  " + preview;
            }
        } else if (tool_name.rfind("browser_", 0) == 0) {
            auto value_for = [&j](const char* key) -> std::string {
                if (j.contains(key) && j[key].is_string()) return j[key].get<std::string>();
                return {};
            };
            std::string value;
            if (tool_name == "browser_start") value = value_for("session");
            else if (tool_name == "browser_open") value = value_for("url");
            else if (tool_name == "browser_navigate") {
                value = value_for("operation");
                std::string url = value_for("url");
                if (!url.empty()) value += " " + url;
            } else if (tool_name == "browser_read_page") value = value_for("mode");
            else if (tool_name == "browser_enable") {
                if (j.contains("groups") && j["groups"].is_array()) {
                    bool first = true;
                    for (const auto& g : j["groups"]) {
                        if (!g.is_string()) continue;
                        if (!first) value += ",";
                        value += g.get<std::string>();
                        first = false;
                    }
                }
            } else if (tool_name == "browser_fill") {
                value = value_for("target");
                std::string fill_value = value_for("value");
                if (!fill_value.empty()) {
                    value += " = " + truncate_utf8_prefix(fill_value, 40);
                }
            } else if (tool_name == "browser_type") {
                value = value_for("target");
                std::string text = value_for("text");
                if (!text.empty()) value += " = " + truncate_utf8_prefix(text, 40);
            } else if (tool_name == "browser_evaluate") {
                value = truncate_utf8_prefix(value_for("code"), 60);
            } else if (tool_name == "browser_network") {
                value = value_for("cmd");
                std::string filter = value_for("filter");
                if (!filter.empty()) value += " " + filter;
            } else {
                value = value_for("target");
                if (value.empty()) value = value_for("session");
            }
            value = truncate_utf8_prefix(value, 60);
            return value.empty() ? tool_name : tool_name + "  " + value;
        }
    } catch (...) {
        // fall through to empty preview → TUI legacy render
    }
    return {};
}

ChatMessage ToolExecutor::format_assistant_tool_calls(const ChatResponse& response) {
    ChatMessage msg;
    msg.role = "assistant";
    msg.content = response.content;
    // Carry reasoning_content forward so the next API call can echo it back.
    msg.reasoning_content = response.reasoning_content;

    nlohmann::json tc_array = nlohmann::json::array();
    for (const auto& tc : response.tool_calls) {
        nlohmann::json tc_obj;
        tc_obj["id"] = tc.id;
        tc_obj["type"] = "function";
        tc_obj["function"]["name"] = tc.function_name;
        tc_obj["function"]["arguments"] = tc.function_arguments;
        tc_array.push_back(tc_obj);
    }
    msg.tool_calls = tc_array;

    return msg;
}

} // namespace acecode
