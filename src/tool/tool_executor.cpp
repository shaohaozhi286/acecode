#include "tool_executor.hpp"
#include "tool_protocol_names.hpp"
#include "../session/output_attachments.hpp"
#include "utils/logger.hpp"
#include "utils/encoding.hpp"
#include "utils/tool_errors.hpp"
#include "utils/utf8_path.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <string_view>

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

constexpr std::array<std::string_view, 3> kScratchPathAliases = {
    "%ACECODE_TMPDIR%",
    "${ACECODE_TMPDIR}",
    "$ACECODE_TMPDIR",
};

char ascii_lower(char value) {
    if (value >= 'A' && value <= 'Z') {
        return static_cast<char>(value - 'A' + 'a');
    }
    return value;
}

size_t find_ascii_case_insensitive(const std::string& value,
                                   std::string_view needle) {
    if (needle.empty() || value.size() < needle.size()) {
        return std::string::npos;
    }
    const size_t limit = value.size() - needle.size();
    for (size_t offset = 0; offset <= limit; ++offset) {
        bool matches = true;
        for (size_t i = 0; i < needle.size(); ++i) {
            if (ascii_lower(value[offset + i]) != ascii_lower(needle[i])) {
                matches = false;
                break;
            }
        }
        if (matches) return offset;
    }
    return std::string::npos;
}

struct ScratchAliasMatch {
    size_t offset = std::string::npos;
    size_t length = 0;
};

ScratchAliasMatch find_first_scratch_alias(const std::string& value) {
    ScratchAliasMatch first;
    for (const auto alias : kScratchPathAliases) {
        const size_t offset = find_ascii_case_insensitive(value, alias);
        if (offset < first.offset) {
            first.offset = offset;
            first.length = alias.size();
        }
    }
    return first;
}

ScratchPathResolution scratch_path_error(const std::string& message) {
    ScratchPathResolution result;
    result.success = false;
    result.error = ToolErrors::invalid_parameter("file_path", message);
    return result;
}

bool tool_allowed_by_policy(const ToolImpl& impl,
                            const ToolCapabilityPolicy* policy) {
    if (impl.defer_loading &&
        (!policy ||
         policy->enabled_deferred_tools.count(impl.definition.name) == 0)) {
        return false;
    }
    if (!policy) return true;
    if (impl.source == ToolSource::Builtin) {
        return !policy->builtin_tools ||
               policy->builtin_tools->count(impl.definition.name) != 0;
    }
    return !policy->mcp_servers ||
           (!impl.source_owner.empty() &&
            policy->mcp_servers->count(impl.source_owner) != 0);
}

ToolResult expert_policy_denied_result(const std::string& tool_name) {
    return ToolResult{
        "[Error] Tool denied by the active expert capability policy: " +
            tool_name,
        false};
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

bool ToolContext::references_scratch_path_alias(const std::string& value) {
    return find_first_scratch_alias(value).offset != std::string::npos;
}

ScratchPathResolution ToolContext::resolve_scratch_path_alias(
    const std::string& file_path) const {
    const ScratchAliasMatch alias = find_first_scratch_alias(file_path);
    if (alias.offset == std::string::npos) {
        ScratchPathResolution result;
        result.path = file_path;
        return result;
    }
    if (alias.offset != 0) {
        return scratch_path_error(
            "ACECODE_TMPDIR must be the first path component when used with file tools.");
    }
    if (scratch_dir.empty()) {
        return scratch_path_error(
            "ACECODE_TMPDIR is unavailable for this tool call; use an absolute path or retry in an active session.");
    }

    size_t suffix_offset = alias.length;
    if (suffix_offset >= file_path.size() ||
        (file_path[suffix_offset] != '/' && file_path[suffix_offset] != '\\')) {
        return scratch_path_error(
            "ACECODE_TMPDIR file paths must include a file name after the alias prefix.");
    }
    while (suffix_offset < file_path.size() &&
           (file_path[suffix_offset] == '/' || file_path[suffix_offset] == '\\')) {
        ++suffix_offset;
    }
    if (suffix_offset >= file_path.size()) {
        return scratch_path_error(
            "ACECODE_TMPDIR file paths must include a file name after the alias prefix.");
    }

    std::string suffix = file_path.substr(suffix_offset);
    if (references_scratch_path_alias(suffix)) {
        return scratch_path_error(
            "ACECODE_TMPDIR may appear only once as the leading file-tool path component.");
    }
    // File-tool aliases accept either shell's separator spelling. Forward
    // slashes are understood by std::filesystem on Windows and POSIX.
    std::replace(suffix.begin(), suffix.end(), '\\', '/');

    namespace fs = std::filesystem;
    const fs::path relative = path_from_utf8(suffix);
    if (relative.is_absolute() || relative.has_root_name() ||
        relative.has_root_directory()) {
        return scratch_path_error(
            "ACECODE_TMPDIR file paths must remain relative to the session scratch directory.");
    }
    for (const auto& component : relative) {
        if (component == "..") {
            return scratch_path_error(
                "ACECODE_TMPDIR file paths cannot contain parent traversal components.");
        }
    }
    if (relative.filename().empty() || relative.filename() == ".") {
        return scratch_path_error(
            "ACECODE_TMPDIR file paths must identify a file beneath the session scratch directory.");
    }

    fs::path root = path_from_utf8(scratch_dir);
    if (root.is_relative()) {
        if (cwd.empty()) {
            return scratch_path_error(
                "ACECODE_TMPDIR cannot be resolved without an absolute scratch directory or working directory.");
        }
        root = path_from_utf8(cwd) / root;
    }
    root = root.lexically_normal();
    if (root.is_relative()) {
        return scratch_path_error(
            "ACECODE_TMPDIR cannot be resolved without an absolute scratch directory or working directory.");
    }

    ScratchPathResolution result;
    result.used_alias = true;
    result.path = path_to_utf8((root / relative).lexically_normal());
    return result;
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

bool ToolExecutor::register_tool(const ToolImpl& tool) {
    std::lock_guard<std::mutex> lk(tools_mu_);
    std::string mapping_error;
    if (!validate_model_tool_name_mappings(&mapping_error)) {
        LOG_ERROR("Refusing tool registration because model tool name mappings are invalid: " +
                  mapping_error);
        return false;
    }

    const auto existing = tools_.find(tool.definition.name);
    if (existing != tools_.end() &&
        (existing->second.source != tool.source ||
         existing->second.source_owner != tool.source_owner)) {
        LOG_WARN("Refusing tool registration collision for " +
                 tool.definition.name);
        return false;
    }

    const std::string public_name =
        model_tool_name_for_native(tool.definition.name);
    for (const auto& [registered_name, impl] : tools_) {
        (void)impl;
        if (registered_name == tool.definition.name) continue;
        if (model_tool_name_for_native(registered_name) == public_name) {
            LOG_WARN("Refusing model-facing tool name collision for " +
                     tool.definition.name + " and " + registered_name +
                     " (public name: " + public_name + ")");
            return false;
        }
    }

    LOG_INFO("Registering tool: " + tool.definition.name);
    tools_[tool.definition.name] = tool;
    return true;
}

bool ToolExecutor::unregister_tool(
    const std::string& name,
    std::optional<std::string> expected_source_owner) {
    std::lock_guard<std::mutex> lk(tools_mu_);
    auto it = tools_.find(name);
    if (it == tools_.end()) return false;
    if (expected_source_owner &&
        it->second.source_owner != *expected_source_owner) {
        LOG_WARN("Refusing tool unregister owner mismatch for " + name);
        return false;
    }
    LOG_INFO("Unregistering tool: " + name);
    tools_.erase(it);
    return true;
}

std::vector<ToolDef> ToolExecutor::get_tool_definitions(
    const ToolCapabilityPolicy* policy) const {
    std::vector<ToolDef> defs;
    std::lock_guard<std::mutex> lk(tools_mu_);
    for (const auto& [name, impl] : tools_) {
        (void)name;
        if (tool_allowed_by_policy(impl, policy)) {
            defs.push_back(impl.definition);
        }
    }
    return defs;
}

std::vector<ToolDef> ToolExecutor::get_tool_definitions_by_source(
    ToolSource source,
    const ToolCapabilityPolicy* policy) const {
    std::vector<ToolDef> defs;
    std::lock_guard<std::mutex> lk(tools_mu_);
    for (const auto& [name, impl] : tools_) {
        (void)name;
        if (impl.source == source && tool_allowed_by_policy(impl, policy)) {
            defs.push_back(impl.definition);
        }
    }
    return defs;
}

std::vector<ToolDef> ToolExecutor::get_model_tool_definitions(
    const ToolCapabilityPolicy* policy) const {
    std::vector<ToolDef> definitions;
    std::string error;
    if (!translate_tool_definitions_for_model(
            get_tool_definitions(policy), definitions, &error)) {
        LOG_ERROR("Unable to build model-facing tool definitions: " + error);
        return {};
    }
    return definitions;
}

std::vector<ToolDef> ToolExecutor::get_model_tool_definitions_by_source(
    ToolSource source,
    const ToolCapabilityPolicy* policy) const {
    std::vector<ToolDef> definitions;
    std::string error;
    if (!translate_tool_definitions_for_model(
            get_tool_definitions_by_source(source, policy), definitions, &error)) {
        LOG_ERROR("Unable to build model-facing tool definitions by source: " + error);
        return {};
    }
    return definitions;
}

std::string ToolExecutor::resolve_model_tool_name_to_native(
    const std::string& model_name) const {
    std::lock_guard<std::mutex> lk(tools_mu_);
    if (tools_.find(model_name) != tools_.end()) return model_name;

    const auto native_alias = native_tool_name_for_public_alias(model_name);
    if (native_alias && tools_.find(*native_alias) != tools_.end()) {
        return *native_alias;
    }
    return model_name;
}

std::vector<RegisteredToolInfo> ToolExecutor::get_registered_tools() const {
    std::vector<RegisteredToolInfo> result;
    std::lock_guard<std::mutex> lk(tools_mu_);
    result.reserve(tools_.size());
    for (const auto& [name, impl] : tools_) {
        (void)name;
        result.push_back({
            impl.definition,
            impl.is_read_only,
            impl.defer_loading,
            impl.source,
            impl.source_owner,
        });
    }
    return result;
}

bool ToolExecutor::is_allowed(const std::string& name,
                              const ToolCapabilityPolicy* policy) const {
    std::lock_guard<std::mutex> lk(tools_mu_);
    const auto it = tools_.find(name);
    return it != tools_.end() && tool_allowed_by_policy(it->second, policy);
}

bool ToolExecutor::is_denied_by_policy(
    const std::string& name,
    const ToolCapabilityPolicy* policy) const {
    std::lock_guard<std::mutex> lk(tools_mu_);
    const auto it = tools_.find(name);
    return it != tools_.end() &&
           !tool_allowed_by_policy(it->second, policy);
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
        const ToolCapabilityPolicy* policy =
            ctx.capability_policy ? &*ctx.capability_policy : nullptr;
        if (!tool_allowed_by_policy(it->second, policy)) {
            LOG_WARN("execute: expert capability policy denied tool '" +
                     tool_name + "'");
            return expert_policy_denied_result(tool_name);
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

bool ToolExecutor::is_deferred(const std::string& name) const {
    std::lock_guard<std::mutex> lk(tools_mu_);
    auto it = tools_.find(name);
    return it != tools_.end() && it->second.defer_loading;
}

std::string ToolExecutor::generate_tools_prompt(
    const ToolCapabilityPolicy* policy) const {
    std::ostringstream oss;
    std::lock_guard<std::mutex> lk(tools_mu_);
    for (const auto& [name, impl] : tools_) {
        (void)name;
        if (!tool_allowed_by_policy(impl, policy)) continue;
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
            if (tool_name == "browser_open") value = value_for("url");
            else if (tool_name == "browser_navigate") {
                value = value_for("action");
                std::string url = value_for("url");
                if (!url.empty()) value += " " + url;
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
            } else if (tool_name == "browser_press") {
                value = value_for("key");
            } else if (tool_name == "browser_drag") {
                value = value_for("from") + " -> " + value_for("to");
            } else if (tool_name == "browser_wait") {
                value = value_for("condition");
            } else if (tool_name == "browser_screenshot") {
                value = value_for("file_name");
            } else {
                value = value_for("target");
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
