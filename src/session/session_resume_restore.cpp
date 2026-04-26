#include "session_resume_restore.hpp"

#include "session_replay.hpp"
#include "../agent_loop.hpp"
#include "../tool/tool_executor.hpp"
#include "../tui_state.hpp"

namespace acecode {

namespace {

bool is_llm_role(const std::string& role) {
    return role == "user" || role == "assistant" ||
           role == "system" || role == "tool";
}

} // namespace

void append_resumed_session_messages(const std::vector<ChatMessage>& messages,
                                     TuiState& state,
                                     AgentLoop& agent_loop,
                                     const ToolExecutor& tools) {
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
            state.conversation.push_back({messages[i + 1].role,
                                          messages[i + 1].content,
                                          true});
            agent_loop.inject_shell_turn(msg.content.substr(1),
                                         messages[i + 1].content,
                                         "",
                                         0);
            ++i;
            continue;
        }

        // Keep provider-facing history canonical. UI-only pseudo-roles such as
        // standalone `tool_result` are rendered, but not sent to providers.
        if (is_llm_role(msg.role)) {
            agent_loop.push_message(msg);
        }
        replay_buffer.push_back(msg);
    }

    flush_replay();
}

} // namespace acecode
