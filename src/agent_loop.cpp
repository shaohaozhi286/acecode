#include "agent_loop.hpp"
#include "prompt/system_prompt.hpp"
#include "utils/logger.hpp"
#include <nlohmann/json.hpp>
#include <mutex>

namespace acecode {

AgentLoop::AgentLoop(LlmProvider& provider, ToolExecutor& tools, AgentCallbacks callbacks,
                     const std::string& cwd)
    : provider_(provider)
    , tools_(tools)
    , callbacks_(std::move(callbacks))
    , cwd_(cwd)
{
}

void AgentLoop::abort() {
    abort_requested_ = true;
}

void AgentLoop::set_callbacks(AgentCallbacks cb) {
    callbacks_ = std::move(cb);
}

void AgentLoop::submit(const std::string& user_message) {
    abort_requested_ = false;

    LOG_INFO("=== submit() user_message: " + log_truncate(user_message, 200));

    // Add user message
    ChatMessage user_msg;
    user_msg.role = "user";
    user_msg.content = user_message;
    messages_.push_back(user_msg);

    if (callbacks_.on_busy_changed) {
        callbacks_.on_busy_changed(true);
    }

    auto tool_defs = tools_.get_tool_definitions();
    LOG_DEBUG("Registered tools: " + std::to_string(tool_defs.size()));

    // Agent loop: keep calling the provider until we get a pure text response
    int turn = 0;
    while (true) {
        ++turn;
        LOG_INFO("--- Agent loop turn " + std::to_string(turn) + ", messages: " + std::to_string(messages_.size()));

        if (abort_requested_) {
            LOG_WARN("Abort requested, breaking loop");
            if (callbacks_.on_message) {
                callbacks_.on_message("system", "[Interrupted]", false);
            }
            break;
        }

        // Build system prompt each turn (dynamic: includes current tools and CWD)
        std::string system_prompt = build_system_prompt(tools_, cwd_);
        LOG_DEBUG("System prompt length: " + std::to_string(system_prompt.size()));

        // Prepare messages with system prompt at front
        std::vector<ChatMessage> messages_with_system;
        ChatMessage sys_msg;
        sys_msg.role = "system";
        sys_msg.content = system_prompt;
        messages_with_system.push_back(sys_msg);
        messages_with_system.insert(messages_with_system.end(), messages_.begin(), messages_.end());

        // Use streaming API
        ChatResponse accumulated;
        accumulated.finish_reason = "stop";
        std::mutex resp_mu;

        auto stream_callback = [&](const StreamEvent& evt) {
            switch (evt.type) {
            case StreamEventType::Delta:
                {
                    std::lock_guard<std::mutex> lk(resp_mu);
                    accumulated.content += evt.content;
                }
                if (callbacks_.on_delta) {
                    callbacks_.on_delta(evt.content);
                }
                break;
            case StreamEventType::ToolCall:
                {
                    std::lock_guard<std::mutex> lk(resp_mu);
                    accumulated.tool_calls.push_back(evt.tool_call);
                }
                break;
            case StreamEventType::Done:
                break;
            case StreamEventType::Error:
                if (callbacks_.on_message) {
                    callbacks_.on_message("error", "[Error] " + evt.error, false);
                }
                break;
            }
        };

        LOG_INFO("Calling chat_stream with " + std::to_string(messages_with_system.size()) + " messages");
        try {
            provider_.chat_stream(messages_with_system, tool_defs, stream_callback, &abort_requested_);
            LOG_INFO("chat_stream returned. content_len=" + std::to_string(accumulated.content.size()) + " tool_calls=" + std::to_string(accumulated.tool_calls.size()));
        } catch (const std::exception& e) {
            LOG_ERROR(std::string("chat_stream exception: ") + e.what());
            if (callbacks_.on_message) {
                callbacks_.on_message("error", std::string("[Error] ") + e.what(), false);
            }
            break;
        }

        if (abort_requested_) {
            if (callbacks_.on_message) {
                callbacks_.on_message("system", "[Interrupted]", false);
            }
            break;
        }

        if (!accumulated.has_tool_calls()) {
            // Pure text response -- conversation turn is done
            LOG_INFO("Pure text response, ending loop. content: " + log_truncate(accumulated.content, 300));
            ChatMessage assistant_msg;
            assistant_msg.role = "assistant";
            assistant_msg.content = accumulated.content;
            messages_.push_back(assistant_msg);

            if (callbacks_.on_message) {
                callbacks_.on_message("assistant", accumulated.content, false);
            }
            break;
        }

        // Assistant wants to call tools
        // Record the assistant message with tool_calls in the history
        messages_.push_back(ToolExecutor::format_assistant_tool_calls(accumulated));

        // Execute each tool call individually, reporting result after each
        LOG_INFO("Processing " + std::to_string(accumulated.tool_calls.size()) + " tool calls");
        for (const auto& tc : accumulated.tool_calls) {
            if (abort_requested_) break;

            LOG_INFO("Tool call: " + tc.function_name + " id=" + tc.id + " args=" + log_truncate(tc.function_arguments, 300));
            // Notify TUI about the tool call
            if (callbacks_.on_message) {
                callbacks_.on_message("tool_call",
                    "[Tool: " + tc.function_name + "] " + tc.function_arguments, true);
            }

            // Ask for user confirmation
            if (callbacks_.on_tool_confirm) {
                bool approved = callbacks_.on_tool_confirm(tc.function_name, tc.function_arguments);
                if (!approved) {
                    ToolResult denied_result{"[User denied tool execution]", false};
                    messages_.push_back(ToolExecutor::format_tool_result(tc.id, denied_result));
                    if (callbacks_.on_message) {
                        callbacks_.on_message("tool_result", "[User denied tool execution]", true);
                    }
                    continue;
                }
            }

            // Execute the tool
            ToolResult result;
            if (tools_.has_tool(tc.function_name)) {
                LOG_DEBUG("Executing tool: " + tc.function_name);
                result = tools_.execute(tc.function_name, tc.function_arguments);
                LOG_INFO("Tool result: success=" + std::string(result.success ? "true" : "false") + " output=" + log_truncate(result.output, 300));
            } else {
                LOG_WARN("Unknown tool: " + tc.function_name);
                result = ToolResult{"Unknown tool: " + tc.function_name, false};
            }

            // Record tool result in conversation and notify TUI immediately
            messages_.push_back(ToolExecutor::format_tool_result(tc.id, result));

            if (callbacks_.on_message) {
                callbacks_.on_message("tool_result", result.output, true);
            }
        }

        // Loop back to call the provider again with the tool results
    }

    if (callbacks_.on_busy_changed) {
        callbacks_.on_busy_changed(false);
    }
}

} // namespace acecode
