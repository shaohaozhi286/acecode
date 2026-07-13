#include "session_markdown_export.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string_view>

namespace acecode::session_export {

namespace {

std::string one_line(std::string value) {
    for (char& c : value) {
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    }
    return value;
}

std::string inline_code(std::string value) {
    for (char& c : value) {
        if (c == '`') c = '\'';
    }
    return value;
}

void append_text_block(std::ostringstream& out, const std::string& value) {
    if (value.empty()) return;
    out << value;
    if (value.back() != '\n') out << '\n';
    out << '\n';
}

std::string role_label(const std::string& role) {
    if (role == "user") return "用户";
    if (role == "assistant") return "助手";
    if (role == "system") return "系统";
    if (role == "tool" || role == "tool_result") return "工具结果";
    if (role == "tool_call") return "工具调用";
    if (role == "error") return "错误";
    return role.empty() ? "消息" : role;
}

std::string message_text(const ChatMessage& message) {
    if (message.role == "user" && message.metadata.is_object()) {
        const auto it = message.metadata.find("display_text");
        if (it != message.metadata.end() && it->is_string() && !it->get<std::string>().empty()) {
            return it->get<std::string>();
        }
    }
    return message.content;
}

void append_json_block(std::ostringstream& out,
                       const std::string& heading,
                       const nlohmann::json& value) {
    if (value.is_null() || value.empty()) return;
    out << "### " << heading << "\n\n```json\n"
        << value.dump(2) << "\n```\n\n";
}

void append_tool_calls(std::ostringstream& out, const nlohmann::json& raw_calls) {
    if (raw_calls.is_null() || raw_calls.empty()) return;
    const auto calls = raw_calls.is_array()
        ? raw_calls
        : nlohmann::json::array({raw_calls});

    out << "### 工具调用参数\n\n";
    for (const auto& call : calls) {
        if (!call.is_object()) {
            out << "```json\n" << call.dump(2) << "\n```\n\n";
            continue;
        }
        const auto function = call.value("function", nlohmann::json::object());
        std::string name = call.value("name", std::string{});
        if (function.is_object() && name.empty()) {
            name = function.value("name", std::string{});
        }
        const std::string id = call.value("id", std::string{});
        out << "- 工具: `" << inline_code(name.empty() ? "未知工具" : name) << "`";
        if (!id.empty()) out << "，调用 ID: `" << inline_code(id) << "`";
        out << "\n";

        nlohmann::json arguments;
        if (function.is_object() && function.contains("arguments")) {
            arguments = function["arguments"];
        } else if (call.contains("arguments")) {
            arguments = call["arguments"];
        }
        if (arguments.is_string()) {
            try {
                arguments = nlohmann::json::parse(arguments.get<std::string>());
            } catch (...) {
                // Keep malformed/raw arguments as a JSON string below.
            }
        }
        if (!arguments.is_null()) {
            out << "```json\n" << arguments.dump(2) << "\n```\n";
        }
        out << '\n';
    }
}

bool is_reserved_windows_name(std::string value) {
    const auto dot = value.find('.');
    value = value.substr(0, dot);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    if (value == "CON" || value == "PRN" || value == "AUX" || value == "NUL") return true;
    if (value.size() == 4 && (value.rfind("COM", 0) == 0 || value.rfind("LPT", 0) == 0)) {
        return value[3] >= '1' && value[3] <= '9';
    }
    return false;
}

} // namespace

std::string sanitize_filename_stem(const std::string& preferred,
                                   const std::string& fallback) {
    std::string result;
    result.reserve(preferred.size());
    for (unsigned char c : preferred) {
        if (c < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' ||
            c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') {
            result.push_back('_');
        } else {
            result.push_back(static_cast<char>(c));
        }
    }
    while (!result.empty() && (result.back() == ' ' || result.back() == '.')) {
        result.pop_back();
    }
    if (result.empty()) result = fallback;
    if (result.empty()) result = "session";
    if (is_reserved_windows_name(result)) result.insert(0, 1, '_');
    return result;
}

std::string choose_markdown_filename(const std::filesystem::path& directory,
                                     const std::string& preferred,
                                     const std::string& fallback) {
    const std::string stem = sanitize_filename_stem(preferred, fallback);
    for (int suffix = 0; suffix < 100000; ++suffix) {
        const std::string filename = suffix == 0
            ? stem + ".md"
            : stem + " (" + std::to_string(suffix + 1) + ").md";
        std::error_code ec;
        if (!std::filesystem::exists(directory / std::filesystem::u8path(filename), ec) && !ec) {
            return filename;
        }
    }
    return stem + " (" + std::to_string(100001) + ").md";
}

std::string build_markdown(const SessionMeta& meta,
                           const std::vector<ChatMessage>& messages) {
    const std::string title = one_line(
        meta.title.empty() ? (meta.summary.empty() ? meta.id : meta.summary) : meta.title);
    std::ostringstream out;
    out << "# " << (title.empty() ? "会话" : title) << "\n\n";
    if (!meta.id.empty()) out << "- 会话 ID: `" << inline_code(meta.id) << "`\n";
    if (!meta.cwd.empty()) out << "- 工作目录: `" << inline_code(meta.cwd) << "`\n";
    if (!meta.created_at.empty()) out << "- 创建时间: `" << inline_code(meta.created_at) << "`\n";
    if (!meta.updated_at.empty()) out << "- 更新时间: `" << inline_code(meta.updated_at) << "`\n";
    if (!meta.provider.empty()) out << "- Provider: `" << inline_code(meta.provider) << "`\n";
    if (!meta.model.empty()) out << "- 模型: `" << inline_code(meta.model) << "`\n";
    out << "\n---\n\n";

    bool wrote_message = false;
    for (const auto& message : messages) {
        if (message.role.empty()) continue;
        wrote_message = true;
        out << "## " << role_label(message.role) << "\n\n";
        if (!message.timestamp.empty()) {
            out << "时间: `" << inline_code(message.timestamp) << "`\n\n";
        }
        append_text_block(out, message_text(message));
        if (message.content_parts.is_array() && !message.content_parts.empty()) {
            append_json_block(out, "结构化内容", message.content_parts);
        }
        if (!message.tool_call_id.empty()) {
            out << "工具调用 ID: `" << inline_code(message.tool_call_id) << "`\n\n";
        }
        append_tool_calls(out, message.tool_calls);
        if (message.metadata.is_object() && !message.metadata.empty()) {
            append_json_block(out, "消息元数据", message.metadata);
        }
    }
    if (!wrote_message) out << "> 会话暂无可见消息。\n";
    return out.str();
}

} // namespace acecode::session_export
