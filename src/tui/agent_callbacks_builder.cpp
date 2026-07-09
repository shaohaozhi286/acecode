#include "tui/agent_callbacks_builder.hpp"

#include <chrono>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include "agent_loop.hpp"
#include "commands/compact.hpp"
#include "tui/tool_row_format.hpp"
#include "tui/tui_helpers.hpp"
#include "tui/clipboard_helpers.hpp"
#include "session/todo_state.hpp"
#include "remote_control/remote_control_service.hpp"
#include "utils/power_inhibitor.hpp"

namespace acecode { namespace tui {

void setup_agent_callbacks(TuiContext& ctx) {
    auto& state = ctx.state;
    auto& screen = ctx.screen;
    auto& agent_aborting = ctx.agent_aborting;

    AgentCallbacks callbacks;

    callbacks.on_message = [&state, &screen](const std::string& role, const std::string& content, bool is_tool) {
        std::lock_guard<std::mutex> lk(state.mu);
        if (!is_tool && role == "assistant" &&
            !state.conversation.empty() &&
            state.conversation.back().role == "assistant" &&
            !state.conversation.back().is_tool) {
            state.conversation.back().content = content;
        } else {
            TuiState::Message m{role, content, is_tool};
            if (role == "tool_call") {
                // 与 main.cpp on_message 一致:push 时算好紧凑预览,执行中
                // 的工具行即显示 `● Bash(args)` 而非原始 JSON。
                const auto parts = parse_tool_row(content, std::string());
                if (!parts.name.empty()) {
                    m.display_override = ToolExecutor::build_tool_call_preview(
                        parts.name, parts.args);
                }
            }
            state.conversation.push_back(std::move(m));
        }
        state.chat_follow_tail = true;
        screen.PostEvent(ftxui::Event::Custom);
    };

    callbacks.on_tool_confirm = [&state, &screen, &agent_aborting](const std::string& tool_name, const std::string& args) -> PermissionResult {
        {
            std::lock_guard<std::mutex> lk(state.mu);
            state.confirm_pending = true;
            state.confirm_tool_name = tool_name;
            state.confirm_tool_args = args;
            state.confirm_focus = 2;
        }
        screen.PostEvent(ftxui::Event::Custom);
        std::unique_lock<std::mutex> lk(state.mu);
        state.confirm_cv.wait(lk, [&state, &agent_aborting] {
            return !state.confirm_pending || agent_aborting.load();
        });
        if (agent_aborting.load()) return PermissionResult::Deny;
        return state.confirm_result;
    };

    callbacks.on_delta = [&state, &screen](const std::string& token) {
        std::lock_guard<std::mutex> lk(state.mu);
        if (state.conversation.empty() ||
            state.conversation.back().role != "assistant" ||
            state.conversation.back().is_tool) {
            state.conversation.push_back({"assistant", "", false});
        }
        state.conversation.back().content += token;
        state.streaming_output_chars += token.size();
        state.chat_follow_tail = true;
        screen.PostEvent(ftxui::Event::Custom);
    };

    callbacks.on_tool_result = [&state, &screen](const ChatMessage& call_msg, const std::string&, const ToolResult& result) {
        std::lock_guard<std::mutex> lk(state.mu);
        for (auto it = state.conversation.rbegin(); it != state.conversation.rend(); ++it) {
            if (it->role == "tool_result" && !it->summary.has_value()) {
                it->summary = result.summary;
                it->hunks = result.hunks;
                break;
            }
        }
        if (!call_msg.display_override.empty()) {
            for (auto it = state.conversation.rbegin(); it != state.conversation.rend(); ++it) {
                if (it->role == "tool_call" && it->display_override.empty()) {
                    it->display_override = call_msg.display_override;
                    break;
                }
            }
        }
        screen.PostEvent(ftxui::Event::Custom);
    };

    callbacks.on_usage = [&state, &screen](const TokenUsage& usage) {
        std::lock_guard<std::mutex> lk(state.mu);
        state.token_status = std::to_string(usage.total_tokens);
        state.last_completion_tokens_authoritative = usage.completion_tokens;
        screen.PostEvent(ftxui::Event::Custom);
    };

    callbacks.on_goal_status = [&state, &screen](const std::string& status) {
        std::lock_guard<std::mutex> lk(state.mu);
        state.goal_status = status;
        screen.PostEvent(ftxui::Event::Custom);
    };

    callbacks.on_todo_updated = [&state, &screen](const nlohmann::json& payload) {
        std::lock_guard<std::mutex> lk(state.mu);
        if (payload.is_object() && payload.contains("todos")) {
            state.todos = todo_items_from_json(payload["todos"]);
        } else {
            state.todos.clear();
        }
        screen.PostEvent(ftxui::Event::Custom);
    };

    callbacks.on_transcript_replace = [&state, &screen](const std::vector<ChatMessage>&, const CompactResult& result) {
        if (!result.performed || result.summary_text.empty()) return;
        std::lock_guard<std::mutex> lk(state.mu);
        state.conversation.push_back({"system", "--- [Compact Checkpoint] ---", false});
        state.conversation.push_back({"system", "[Conversation summary]\n" + result.summary_text, false});
        state.chat_follow_tail = true;
        screen.PostEvent(ftxui::Event::Custom);
    };

    callbacks.on_stream_retry_reset = [&state, &screen]() {
        std::lock_guard<std::mutex> lk(state.mu);
        if (!state.conversation.empty() &&
            state.conversation.back().role == "assistant" &&
            !state.conversation.back().is_tool) {
            state.conversation.pop_back();
        }
        state.streaming_output_chars = 0;
        screen.PostEvent(ftxui::Event::Custom);
    };

    callbacks.on_tool_progress_start = [&state, &screen](const std::string& tool_name, const std::string& cmd_preview) {
        {
            std::lock_guard<std::mutex> lk(state.mu);
            state.tool_running = true;
            state.tool_progress = {};
            state.tool_progress.tool_name = tool_name;
            state.tool_progress.command_preview = cmd_preview;
            state.tool_progress.start_time = std::chrono::steady_clock::now();
            state.last_tool_post_event_time = std::chrono::steady_clock::now();
        }
        screen.PostEvent(ftxui::Event::Custom);
    };

    callbacks.on_tool_progress_update = [&state, &screen](const std::vector<std::string>& tail_snapshot, const std::string& current_partial, size_t total_bytes, int total_lines) {
        bool should_post = false;
        {
            std::lock_guard<std::mutex> lk(state.mu);
            state.tool_progress.tail_lines = tail_snapshot;
            state.tool_progress.current_partial = current_partial;
            state.tool_progress.total_bytes = total_bytes;
            state.tool_progress.total_lines = total_lines;
            auto now = std::chrono::steady_clock::now();
            if (now - state.last_tool_post_event_time > std::chrono::milliseconds(150)) {
                state.last_tool_post_event_time = now;
                should_post = true;
            }
        }
        if (should_post) screen.PostEvent(ftxui::Event::Custom);
    };

    callbacks.on_tool_progress_end = [&state, &screen]() {
        {
            std::lock_guard<std::mutex> lk(state.mu);
            state.tool_running = false;
            state.tool_progress = {};
        }
        screen.PostEvent(ftxui::Event::Custom);
    };

    callbacks.on_busy_changed = [&state, &screen, &ctx](bool busy) {
        acecode::note_process_session_busy("tui-main", busy);
        std::unique_lock<std::mutex> lk(state.mu);
        if (busy && !state.is_waiting) {
            state.current_thinking_phrase = get_random_thinking_phrase(is_user_chinese(state));
            state.thinking_start_time = std::chrono::steady_clock::now();
            state.streaming_output_chars = 0;
            state.last_completion_tokens_authoritative = 0;
        }
        state.is_waiting = busy;
        if (!busy) {
            auto& rc_hub = acecode::rc::remote_control_service().hub();
            if (rc_hub.enabled()) {
                std::size_t cursor = rc_hub.forward_cursor();
                if (cursor > state.conversation.size()) cursor = state.conversation.size();
                for (std::size_t i = cursor; i < state.conversation.size(); ++i) {
                    const auto& m = state.conversation[i];
                    if (m.role == "assistant" && !m.is_tool) rc_hub.notify_assistant_text(m.content);
                }
                rc_hub.set_forward_cursor(state.conversation.size());
            }
        }
        if (!busy && !state.pending_queue.empty()) {
            std::string next_prompt = state.pending_queue.front();
            state.pending_queue.erase(state.pending_queue.begin());
            UserInput next_input;
            bool has_structured_input = false;
            if (!state.pending_structured_queue.empty() &&
                state.pending_structured_queue.front().display_text == next_prompt) {
                next_input = state.pending_structured_queue.front();
                state.pending_structured_queue.pop_front();
                has_structured_input = true;
            }
            state.conversation.push_back({"user", next_prompt, false});
            if (state.drag_scrollbar_phase == TuiState::DragScrollbarPhase::Idle) state.chat_follow_tail = true;
            state.current_thinking_phrase = get_random_thinking_phrase(is_user_chinese(state));
            state.thinking_start_time = std::chrono::steady_clock::now();
            state.streaming_output_chars = 0;
            state.last_completion_tokens_authoritative = 0;
            state.is_waiting = true;
            lk.unlock();
            ctx.coordinate_mcp_before_first_turn();
            lk.lock();
            if (has_structured_input) ctx.agent_loop.submit(next_input);
            else ctx.agent_loop.submit(next_prompt);
        }
        screen.PostEvent(ftxui::Event::Custom);
    };

    ctx.agent_loop.set_callbacks(callbacks);

    acecode::rc::remote_control_service().hub().set_inbound_submit(
        [&state, &screen, &ctx](const std::string& text) {
            std::unique_lock<std::mutex> lk(state.mu);
            if (state.is_waiting) {
                state.pending_queue.push_back(text);
            } else {
                state.conversation.push_back({"user", text, false});
                state.chat_follow_tail = true;
                state.current_thinking_phrase = get_random_thinking_phrase(is_user_chinese(state));
                state.thinking_start_time = std::chrono::steady_clock::now();
                state.streaming_output_chars = 0;
                state.last_completion_tokens_authoritative = 0;
                state.is_waiting = true;
                lk.unlock();
                ctx.coordinate_mcp_before_first_turn();
                lk.lock();
                ctx.agent_loop.submit(text);
            }
            screen.PostEvent(ftxui::Event::Custom);
        });
}

}} // namespace acecode::tui
