#include "session_resume_restore.hpp"

#include "compact_checkpoint.hpp"
#include "session_replay.hpp"
#include "session_rewind.hpp"
#include "tool_result_storage.hpp"
#include "turn_timing.hpp"
#include "../agent_loop.hpp"
#include "../tool/mtime_tracker.hpp"
#include "../tool/tool_executor.hpp"
#include "../tui_state.hpp"
#include "../utils/text_file_buffer.hpp"

#include <map>
#include <optional>

namespace acecode {

namespace {

constexpr const char* kFileUnchangedStubPrefix = "File unchanged since last read.";

bool is_llm_role(const std::string& role) {
    return role == "user" || role == "assistant" ||
           role == "system" || role == "tool";
}

bool is_transcript_only_message(const ChatMessage& msg) {
    return msg.metadata.is_object() &&
           msg.metadata.value("transcript_only", false);
}

std::optional<std::pair<std::size_t, CompactCheckpoint>>
latest_valid_compact_checkpoint(const std::vector<ChatMessage>& messages) {
    for (std::size_t i = messages.size(); i > 0; --i) {
        auto checkpoint = decode_compact_checkpoint(messages[i - 1]);
        if (checkpoint.has_value()) {
            return std::make_pair(i - 1, std::move(*checkpoint));
        }
    }
    return std::nullopt;
}

void append_agent_model_messages(const std::vector<ChatMessage>& messages,
                                 AgentLoop& agent_loop) {
    for (std::size_t i = 0; i < messages.size(); ++i) {
        const auto& msg = messages[i];
        if (is_file_checkpoint_message(msg)) continue;
        if (is_content_replacement_message(msg)) continue;
        if (is_turn_timing_message(msg)) continue;
        if (is_compact_checkpoint_message(msg)) continue;

        const bool is_shell_user =
            (msg.role == "user" && !msg.content.empty() && msg.content[0] == '!');
        const bool next_is_result =
            (i + 1 < messages.size() && messages[i + 1].role == "tool_result");
        if (is_shell_user && next_is_result) {
            agent_loop.inject_shell_turn(msg.content.substr(1),
                                         messages[i + 1].content,
                                         "",
                                         0);
            ++i;
            continue;
        }

        if (is_llm_role(msg.role) && !is_transcript_only_message(msg)) {
            agent_loop.push_message(msg);
        }
    }
}

struct FileToolUse {
    std::string name;
    std::string path;
    std::string content;
    bool partial_read_request = false;
};

bool starts_with(const std::string& value, const char* prefix) {
    return value.rfind(prefix, 0) == 0;
}

bool looks_like_failed_tool_result(const ChatMessage& msg) {
    return starts_with(msg.content, "[Error]") ||
           starts_with(msg.content, "[Interrupted]") ||
           starts_with(msg.content, "[Doom-loop guard]");
}

std::optional<std::string> json_string_field(const nlohmann::json& object,
                                             const char* name) {
    if (!object.is_object() || !object.contains(name) || !object[name].is_string()) {
        return std::nullopt;
    }
    return object[name].get<std::string>();
}

std::optional<FileToolUse> parse_file_tool_use(const nlohmann::json& tool_call) {
    if (!tool_call.is_object() ||
        !tool_call.contains("function") || !tool_call["function"].is_object()) {
        return std::nullopt;
    }

    const auto& fn = tool_call["function"];
    auto name = json_string_field(fn, "name");
    auto arguments_text = json_string_field(fn, "arguments");
    if (!name || !arguments_text) return std::nullopt;
    if (*name != "file_read" && *name != "file_write" && *name != "file_edit") {
        return std::nullopt;
    }

    nlohmann::json args = nlohmann::json::parse(*arguments_text, nullptr, false);
    auto path = json_string_field(args, "file_path");
    if (!path || path->empty()) return std::nullopt;

    FileToolUse use;
    use.name = *name;
    use.path = *path;
    if (*name == "file_read") {
        use.partial_read_request =
            args.is_object() &&
            (args.contains("start_line") ||
             args.contains("end_line") ||
             args.contains("byte_offset") ||
             args.contains("max_bytes"));
    } else if (*name == "file_write") {
        auto content = json_string_field(args, "content");
        if (!content) return std::nullopt;
        use.content = normalize_text_to_lf(*content);
    }
    return use;
}

bool read_footer_is_lossy(const std::string& content) {
    return content.find("lossy=\"true\"") != std::string::npos ||
           content.find("editable=\"false\"") != std::string::npos;
}

bool read_footer_is_partial(const std::string& content) {
    const size_t footer = content.rfind("<acecode-read-metadata");
    if (footer == std::string::npos) return false;
    return content.find("partial=\"true\"", footer) != std::string::npos ||
           content.find("truncated=\"true\"", footer) != std::string::npos;
}

std::string strip_read_metadata_footer(std::string content) {
    const std::string marker = "\n<acecode-read-metadata";
    size_t marker_pos = content.rfind(marker);
    if (marker_pos == std::string::npos && starts_with(content, "<acecode-read-metadata")) {
        marker_pos = 0;
    }
    if (marker_pos != std::string::npos) {
        content.resize(marker_pos);
    }

    const std::string large_hint_prefix = "\n[hint: file is large (";
    size_t hint_pos = content.rfind(large_hint_prefix);
    if (hint_pos != std::string::npos &&
        content.find("). Consider using start_line / end_line to narrow the read next time.]",
                     hint_pos) != std::string::npos) {
        content.resize(hint_pos);
    }
    return content;
}

void restore_file_tool_state(const FileToolUse& use, const ChatMessage& result) {
    if (looks_like_failed_tool_result(result)) return;

    if (use.name == "file_read") {
        if (use.partial_read_request) return;
        if (starts_with(result.content, kFileUnchangedStubPrefix)) return;
        if (read_footer_is_lossy(result.content)) return;
        if (read_footer_is_partial(result.content)) return;

        FileReadEditMetadata metadata;
        MtimeTracker::instance().seed_transcript_read_baseline(
            use.path,
            strip_read_metadata_footer(result.content),
            metadata);
        return;
    }

    if (use.name == "file_write") {
        FileReadEditMetadata metadata;
        MtimeTracker::instance().seed_transcript_read_baseline(
            use.path,
            use.content,
            metadata);
        return;
    }

    if (use.name == "file_edit") {
        auto read_result = read_text_file_buffer(use.path);
        if (read_result.success && !read_result.buffer.metadata.lossy) {
            MtimeTracker::instance().record_write(use.path, read_result.buffer.text);
        }
    }
}

} // namespace

void restore_file_tool_state_from_messages(const std::vector<ChatMessage>& messages) {
    std::map<std::string, FileToolUse> file_tool_uses;

    for (const auto& msg : messages) {
        if (msg.role != "assistant" || !msg.tool_calls.is_array()) continue;
        for (const auto& tool_call : msg.tool_calls) {
            if (!tool_call.is_object() ||
                !tool_call.contains("id") || !tool_call["id"].is_string()) {
                continue;
            }
            auto use = parse_file_tool_use(tool_call);
            if (!use.has_value()) continue;
            file_tool_uses[tool_call["id"].get<std::string>()] = std::move(*use);
        }
    }

    for (const auto& msg : messages) {
        if (msg.role != "tool" || msg.tool_call_id.empty()) continue;
        auto it = file_tool_uses.find(msg.tool_call_id);
        if (it == file_tool_uses.end()) continue;
        restore_file_tool_state(it->second, msg);
    }
}

void append_resumed_session_messages(const std::vector<ChatMessage>& messages,
                                     TuiState& state,
                                     AgentLoop& agent_loop,
                                     const ToolExecutor& tools) {
    restore_file_tool_state_from_messages(messages);

    agent_loop.clear_messages();
    if (auto checkpoint = latest_valid_compact_checkpoint(messages)) {
        append_agent_model_messages(checkpoint->second.replacement_history, agent_loop);
        if (checkpoint->first + 1 < messages.size()) {
            std::vector<ChatMessage> suffix(
                messages.begin() + static_cast<std::ptrdiff_t>(checkpoint->first + 1),
                messages.end());
            append_agent_model_messages(suffix, agent_loop);
        }
    } else {
        append_agent_model_messages(messages, agent_loop);
    }

    std::vector<ChatMessage> replay_buffer;
    auto flush_replay = [&]() {
        if (replay_buffer.empty()) return;
        auto rows = replay_session_messages(replay_buffer, tools);
        for (auto& row : rows) {
            state.conversation.push_back(std::move(row));
        }
        replay_buffer.clear();
    };

    for (size_t i = 0; i < messages.size(); ++i) {
        const auto& msg = messages[i];
        if (is_file_checkpoint_message(msg)) {
            continue;
        }
        if (is_content_replacement_message(msg)) {
            continue;
        }
        if (is_turn_timing_message(msg)) {
            continue;
        }
        if (is_compact_checkpoint_message(msg)) {
            continue;
        }

        // Shell mode persists a UI-only pair: user content starts with '!'
        // followed by a `tool_result` pseudo-role. The TUI should show the pair
        // as-is, while the LLM context gets the XML-tagged shell turn via
        // AgentLoop::inject_shell_turn.
        bool is_shell_user =
            (msg.role == "user" && !msg.content.empty() && msg.content[0] == '!');
        bool next_is_result =
            (i + 1 < messages.size() && messages[i + 1].role == "tool_result");
        if (is_shell_user && next_is_result) {
            flush_replay();
            state.conversation.push_back({msg.role, msg.content, false});
            // 把落盘的 "tool_result"(伪角色)翻译为运行时使用的
            // "user_shell_output",让 resume 后的 chat 视图与实时 `!cmd` 行为
            // 一致——全量显示用户主动跑的命令输出,不走 fold/summary 路径。
            TuiState::Message shell_row;
            shell_row.role = "user_shell_output";
            shell_row.content = messages[i + 1].content;
            shell_row.is_tool = true;
            state.conversation.push_back(std::move(shell_row));
            ++i;
            continue;
        }

        replay_buffer.push_back(msg);
    }

    flush_replay();
}

} // namespace acecode
