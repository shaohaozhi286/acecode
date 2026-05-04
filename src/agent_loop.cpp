#include "agent_loop.hpp"
#include "prompt/system_prompt.hpp"
#include "utils/logger.hpp"
#include "utils/stream_processing.hpp"
#include "commands/compact.hpp"
#include "session/tool_metadata_codec.hpp"
#include "session/session_rewind.hpp"
#include "web/message_payload.hpp"
#include "web/tool_event_payload.hpp"
#include <nlohmann/json.hpp>
#include <chrono>
#include <mutex>
#include <future>
#include <algorithm>
#include <thread>
#include <sstream>
#include <deque>

namespace acecode {

AgentLoop::AgentLoop(ProviderAccessor provider_accessor, ToolExecutor& tools,
                     AgentCallbacks callbacks, const std::string& cwd,
                     PermissionManager& permissions)
    : provider_accessor_(std::move(provider_accessor))
    , tools_(tools)
    , callbacks_(std::move(callbacks))
    , cwd_(cwd)
    , permissions_(permissions)
    , path_validator_(cwd, permissions.is_dangerous())
{
    worker_thread_ = std::thread(&AgentLoop::worker_main, this);
}

AgentLoop::~AgentLoop() {
    shutdown();
}

void AgentLoop::dispatch_message(const std::string& role,
                                  const std::string& content,
                                  bool is_tool) {
    if (callbacks_.on_message) {
        callbacks_.on_message(role, content, is_tool);
    }
    // Web 协议给每条 message 带稳定 id:user 走持久 uuid(走另一路径
    // 直接 emit,见 run_agent),其它角色 lazy sha1(role + " " + content
    // + " " + timestamp)。这里 timestamp 默认空字符串,跟磁盘上 JSONL
    // 重读时算出来的 ID 保持一致(JSONL 里 assistant 消息也没 timestamp)。
    ChatMessage tmp;
    tmp.role    = role;
    tmp.content = content;
    events_.emit(SessionEventKind::Message,
        nlohmann::json{
            {"role", role}, {"content", content}, {"is_tool", is_tool},
            {"id", web::compute_message_id(tmp)}});
}

void AgentLoop::abort() {
    abort_requested_ = true;
}

void AgentLoop::shutdown() {
    {
        std::lock_guard<std::mutex> lk(queue_mu_);
        shutdown_requested_ = true;
    }
    abort_requested_ = true;
    queue_cv_.notify_one();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

void AgentLoop::set_callbacks(AgentCallbacks cb) {
    callbacks_ = std::move(cb);
}

void AgentLoop::worker_main() {
    while (true) {
        WorkerTask task;
        {
            std::unique_lock<std::mutex> lk(queue_mu_);
            queue_cv_.wait(lk, [this] {
                return !task_queue_.empty() || shutdown_requested_;
            });
            if (shutdown_requested_) return;
            task = std::move(task_queue_.front());
            task_queue_.pop();
        }
        switch (task.kind) {
        case WorkerTask::Kind::Chat:
            run_agent(task.payload);
            break;
        case WorkerTask::Kind::Shell:
            run_shell(task.payload);
            break;
        }
    }
}

void AgentLoop::submit(const std::string& user_message) {
    {
        std::lock_guard<std::mutex> lk(queue_mu_);
        task_queue_.push(WorkerTask{WorkerTask::Kind::Chat, user_message});
    }
    queue_cv_.notify_one();
}

void AgentLoop::submit_shell(std::string command) {
    {
        std::lock_guard<std::mutex> lk(queue_mu_);
        task_queue_.push(WorkerTask{WorkerTask::Kind::Shell, std::move(command)});
    }
    queue_cv_.notify_one();
}

void AgentLoop::inject_shell_turn(const std::string& cmd,
                                  const std::string& stdout_text,
                                  const std::string& stderr_text,
                                  int exit_code) {
    ChatMessage msg;
    msg.role = "user";
    std::ostringstream oss;
    oss << "<bash-input>" << cmd << "</bash-input>\n"
        << "<bash-stdout>" << stdout_text << "</bash-stdout>\n"
        << "<bash-stderr>" << stderr_text << "</bash-stderr>\n"
        << "<bash-exit-code>" << exit_code << "</bash-exit-code>";
    msg.content = oss.str();
    messages_.push_back(std::move(msg));
}

void AgentLoop::run_agent(const std::string& user_message) {
    abort_requested_ = false;
    busy_ = true;

    LOG_INFO("=== submit() user_message: " + log_truncate(user_message, 200));

    // Add user message
    ChatMessage user_msg;
    user_msg.role = "user";
    user_msg.content = user_message;
    ensure_user_message_identity(user_msg);
    messages_.push_back(user_msg);
    if (session_manager_) {
        session_manager_->on_message(user_msg);
        session_manager_->begin_user_turn_checkpoint(user_msg.uuid);
    }
    events_.emit(SessionEventKind::Message,
        nlohmann::json{{"role", "user"}, {"content", user_message},
                       {"is_tool", false}, {"id", user_msg.uuid}});

    if (callbacks_.on_busy_changed) {
        callbacks_.on_busy_changed(true);
    }
    events_.emit(SessionEventKind::BusyChanged, nlohmann::json{{"busy", true}});

    auto tool_defs = tools_.get_tool_definitions();
    LOG_DEBUG("Registered tools: " + std::to_string(tool_defs.size()));

    // Agent loop termination protocol (see openspec/changes/align-loop-with-hermes):
    //   - terminator_fired = true → model called task_complete ⇒ exit
    //   - text-only reply (zero tool calls) ⇒ exit (matches hermes-agent /
    //     claudecodehaha; the user re-prompts manually if the model hedged)
    //   - total_iterations ≥ max → hard cap ⇒ emit system message and exit
    //   - abort_requested_ ⇒ exit immediately, [Interrupted] system message
    int total_iterations = 0;
    bool terminator_fired = false;
    std::string terminator_reason;

    const int max_iter = loop_cfg_.max_iterations;

    while (!abort_requested_ && !terminator_fired && total_iterations < max_iter) {
        ++total_iterations;
        LOG_INFO("--- Agent loop turn " + std::to_string(total_iterations) +
                 ", messages: " + std::to_string(messages_.size()));

        if (abort_requested_) {
            LOG_WARN("Abort requested, breaking loop");
            dispatch_message("system", "[Interrupted]", false);
            break;
        }

        // Auto-compact check: prefer API-reported token count, fallback to estimate
        if (should_auto_compact(messages_, context_window_, last_api_prompt_tokens_.load())) {
            LOG_INFO("Auto-compact triggered: estimated tokens exceed threshold (context_window=" + std::to_string(context_window_) + ")");
            if (callbacks_.on_auto_compact) {
                callbacks_.on_auto_compact();
            }
        }

        // Build system prompt each turn (dynamic: includes current tools and CWD)
        std::string system_prompt = build_system_prompt(
            tools_, cwd_, skill_registry_, memory_registry_,
            memory_cfg_, project_instructions_cfg_);
        LOG_DEBUG("System prompt length: " + std::to_string(system_prompt.size()));

        // Prepare messages with system prompt at front, filtering out meta messages
        auto api_messages = normalize_messages_for_api(messages_);
        std::vector<ChatMessage> messages_with_system;
        ChatMessage sys_msg;
        sys_msg.role = "system";
        sys_msg.content = system_prompt;
        messages_with_system.push_back(sys_msg);
        messages_with_system.insert(messages_with_system.end(), api_messages.begin(), api_messages.end());

        // Use streaming API
        ChatResponse accumulated;
        accumulated.finish_reason = "stop";
        std::mutex resp_mu;

        auto stream_callback = [&accumulated, &resp_mu, this](const StreamEvent& evt) {
            switch (evt.type) {
            case StreamEventType::Delta:
                {
                    std::lock_guard<std::mutex> lk(resp_mu);
                    accumulated.content += evt.content;
                }
                if (callbacks_.on_delta) {
                    callbacks_.on_delta(evt.content);
                }
                events_.emit(SessionEventKind::Token, nlohmann::json{{"text", evt.content}});
                break;
            case StreamEventType::ReasoningDelta:
                {
                    std::lock_guard<std::mutex> lk(resp_mu);
                    accumulated.reasoning_content += evt.content;
                }
                // Future TUI hook (e.g. a "Thinking..." panel) can subscribe
                // here. Today we only accumulate so format_assistant_tool_calls
                // and the empty-turn branch can echo it back to DeepSeek.
                events_.emit(SessionEventKind::Reasoning, nlohmann::json{{"text", evt.content}});
                break;
            case StreamEventType::ToolCall:
                {
                    std::lock_guard<std::mutex> lk(resp_mu);
                    accumulated.tool_calls.push_back(evt.tool_call);
                }
                break;
            case StreamEventType::Done:
                break;
            case StreamEventType::Usage:
                last_api_prompt_tokens_.store(evt.usage.prompt_tokens);
                if (callbacks_.on_usage) {
                    callbacks_.on_usage(evt.usage);
                }
                events_.emit(SessionEventKind::Usage, nlohmann::json{
                    {"prompt_tokens", evt.usage.prompt_tokens},
                    {"completion_tokens", evt.usage.completion_tokens},
                    {"total_tokens", evt.usage.total_tokens},
                    {"has_data", evt.usage.has_data},
                });
                break;
            case StreamEventType::Error:
                dispatch_message("error", "[Error] " + evt.error, false);
                break;
            }
        };

        LOG_INFO("Calling chat_stream with " + std::to_string(messages_with_system.size()) + " messages");
        // 每轮 turn 开始时拿一份 provider 快照 —— main.cpp 此时可能正在替换 provider,
        // 但我们这一轮拿到的 shared_ptr 会让老 provider 活到本轮跑完(design D4)。
        std::shared_ptr<LlmProvider> provider_snapshot;
        if (provider_accessor_) provider_snapshot = provider_accessor_();
        if (!provider_snapshot) {
            LOG_ERROR("provider_accessor returned null; aborting turn");
            dispatch_message("error", "[Error] provider unavailable", false);
            break;
        }
        try {
            provider_snapshot->chat_stream(messages_with_system, tool_defs, stream_callback, &abort_requested_);
            LOG_INFO("chat_stream returned. content_len=" + std::to_string(accumulated.content.size()) + " tool_calls=" + std::to_string(accumulated.tool_calls.size()));
        } catch (const std::exception& e) {
            LOG_ERROR(std::string("chat_stream exception: ") + e.what());
            dispatch_message("error", std::string("[Error] ") + e.what(), false);
            break;
        }

        if (abort_requested_) {
            dispatch_message("system", "[Interrupted]", false);
            break;
        }

        if (!accumulated.usage.has_data && callbacks_.on_usage &&
            (!accumulated.content.empty() || !accumulated.tool_calls.empty())) {
            TokenUsage estimated_usage;
            estimated_usage.prompt_tokens = estimate_message_tokens(messages_with_system);

            ChatMessage estimated_response;
            if (accumulated.has_tool_calls()) {
                estimated_response = ToolExecutor::format_assistant_tool_calls(accumulated);
            } else {
                estimated_response.role = "assistant";
                estimated_response.content = accumulated.content;
                estimated_response.reasoning_content = accumulated.reasoning_content;
            }

            estimated_usage.completion_tokens = estimate_message_tokens({estimated_response});
            estimated_usage.total_tokens = estimated_usage.prompt_tokens + estimated_usage.completion_tokens;
            estimated_usage.has_data = false;
            // Note: don't set last_api_prompt_tokens_ here — keep it 0 so
            // should_auto_compact knows this is an estimate, not API data.
            callbacks_.on_usage(estimated_usage);
        }

        if (!accumulated.has_tool_calls()) {
            // Empty turn: no tool calls → end the loop. This matches
            // hermes-agent (run_agent.py:9823) and claudecodehaha. When a
            // non-Claude model hedges with "Would you like me to continue?",
            // the loop ends and the user re-prompts manually.
            LOG_INFO("Text-only response; ending loop. content: " + log_truncate(accumulated.content, 300));
            ChatMessage assistant_msg;
            assistant_msg.role = "assistant";
            assistant_msg.content = accumulated.content;
            // Echo reasoning back on the next turn so DeepSeek thinking-mode
            // doesn't 400. Empty for non-reasoning models — no-op.
            assistant_msg.reasoning_content = accumulated.reasoning_content;
            messages_.push_back(assistant_msg);
            if (session_manager_) session_manager_->on_message(assistant_msg);

            dispatch_message("assistant", accumulated.content, false);
            break;
        }

        // Assistant wants to call tools
        // Record the assistant message with tool_calls in the history
        auto tc_msg = ToolExecutor::format_assistant_tool_calls(accumulated);
        messages_.push_back(tc_msg);
        if (session_manager_) session_manager_->on_message(tc_msg);

        // Partition tool calls into read-only (parallelizable) and write (serial) groups
        LOG_INFO("Processing " + std::to_string(accumulated.tool_calls.size()) + " tool calls");

        struct ToolCallEntry {
            size_t original_index;
            const ToolCall* tc;
            bool is_read_only;
        };

        std::vector<ToolCallEntry> read_entries, write_entries;
        for (size_t i = 0; i < accumulated.tool_calls.size(); ++i) {
            const auto& tc = accumulated.tool_calls[i];
            bool ro = tools_.is_read_only(tc.function_name);
            ToolCallEntry entry{i, &tc, ro};
            if (ro) {
                read_entries.push_back(entry);
            } else {
                write_entries.push_back(entry);
            }
        }

        LOG_INFO("Partitioned: " + std::to_string(read_entries.size()) + " read-only, " +
                 std::to_string(write_entries.size()) + " write");

        // Results array indexed by original position
        std::vector<ToolResult> results(accumulated.tool_calls.size());
        std::vector<bool> result_ready(accumulated.tool_calls.size(), false);

        // Helper: extract context from a tool call
        auto extract_context = [](const ToolCall& tc, std::string& ctx_path, std::string& ctx_command) {
            try {
                auto args_json = nlohmann::json::parse(tc.function_arguments);
                if (args_json.contains("file_path") && args_json["file_path"].is_string()) {
                    ctx_path = args_json["file_path"].get<std::string>();
                } else if (args_json.contains("path") && args_json["path"].is_string()) {
                    ctx_path = args_json["path"].get<std::string>();
                }
                if (args_json.contains("command") && args_json["command"].is_string()) {
                    ctx_command = args_json["command"].get<std::string>();
                }
            } catch (...) {}
        };

        // Helper: execute a single tool (for both parallel and serial use).
        // If `tool_ctx` is provided, streaming/abort hooks are forwarded to the tool.
        auto execute_single_tool = [this](const std::string& tool_name,
                                          const std::string& tool_args,
                                          const std::string& ctx_path,
                                          const ToolContext& tool_ctx = ToolContext{}) -> ToolResult {
            // Path safety validation (for file tools, not bash)
            if (!ctx_path.empty() && tool_name != "bash") {
                std::string path_error = path_validator_.validate(ctx_path);
                if (!path_error.empty()) {
                    LOG_WARN("Path validation failed: " + path_error);
                    return ToolResult{"[Error] " + path_error, false};
                }
            }

            // Execute the tool
            if (tools_.has_tool(tool_name)) {
                LOG_DEBUG("Executing tool: " + tool_name);
                try {
                    ToolResult result = tools_.execute(tool_name, tool_args, tool_ctx);
                    LOG_INFO("Tool result: success=" + std::string(result.success ? "true" : "false") +
                             " output=" + log_truncate(result.output, 300));
                    return result;
                } catch (const std::exception& e) {
                    LOG_ERROR("Tool execution error: " + std::string(e.what()));
                    return ToolResult{"[Error] Tool execution failed: " + std::string(e.what()), false};
                }
            } else {
                LOG_WARN("Unknown tool: " + tool_name);
                return ToolResult{"Unknown tool: " + tool_name, false};
            }
        };

        // Phase 1: Execute read-only tools in parallel
        if (!read_entries.empty() && !abort_requested_) {
            // Notify TUI about all read-only tool calls
            for (const auto& entry : read_entries) {
                dispatch_message("tool_call",
                        "[Tool: " + entry.tc->function_name + "] " + entry.tc->function_arguments, true);
            }

            unsigned int max_concurrency = std::min(
                static_cast<unsigned int>(4),
                std::max(static_cast<unsigned int>(1), std::thread::hardware_concurrency()));

            LOG_DEBUG("Parallel execution with max_concurrency=" + std::to_string(max_concurrency));

            // Launch async tasks in batches respecting concurrency limit
            size_t i = 0;
            while (i < read_entries.size() && !abort_requested_) {
                size_t batch_end = std::min(i + max_concurrency, read_entries.size());
                std::vector<std::future<ToolResult>> futures;

                for (size_t j = i; j < batch_end; ++j) {
                    const auto& entry = read_entries[j];
                    std::string t_name = entry.tc->function_name;
                    std::string t_args = entry.tc->function_arguments;
                    std::string t_path;
                    std::string t_cmd;
                    extract_context(*entry.tc, t_path, t_cmd);
                    ToolContext t_ctx;
                    t_ctx.cwd = cwd_;
                    t_ctx.abort_flag = &abort_requested_;
                    if (session_manager_) {
                        t_ctx.track_file_write_before = [this](const std::string& path) {
                            if (session_manager_) {
                                session_manager_->track_file_write_before(path);
                            }
                        };
                    }
                    // AskUserQuestion 工具(read-only)走这条并行路径,需要这个回调
                    // 才能把 questions 推给前端;漏注入会让 daemon 工厂版直接 fail-fast。
                    if (ask_prompter_) {
                        AskUserQuestionPrompter* p = ask_prompter_;
                        std::atomic<bool>* abort_flag_ptr = &abort_requested_;
                        t_ctx.ask_user_questions =
                            [p, abort_flag_ptr](const nlohmann::json& questions_payload) -> nlohmann::json {
                                AskUserQuestionResponse resp = p->prompt(questions_payload, abort_flag_ptr);
                                nlohmann::json out;
                                out["cancelled"] = resp.cancelled;
                                nlohmann::json arr = nlohmann::json::array();
                                for (const auto& a : resp.answers) {
                                    nlohmann::json item;
                                    item["question_id"] = a.question_id;
                                    item["selected"]    = a.selected;
                                    item["custom_text"] = a.custom_text;
                                    arr.push_back(std::move(item));
                                }
                                out["answers"] = std::move(arr);
                                return out;
                            };
                    }
                    futures.push_back(std::async(std::launch::async,
                        [&execute_single_tool, t_name, t_args, t_path, t_ctx]() {
                            return execute_single_tool(t_name, t_args, t_path, t_ctx);
                        }));
                }

                for (size_t j = 0; j < futures.size(); ++j) {
                    size_t idx = read_entries[i + j].original_index;
                    try {
                        results[idx] = futures[j].get();
                    } catch (const std::exception& e) {
                        results[idx] = ToolResult{"[Error] " + std::string(e.what()), false};
                    }
                    result_ready[idx] = true;

                    // Report result to TUI
                    dispatch_message("tool_result", results[idx].output, true);
                    if (callbacks_.on_tool_result) {
                        const auto& tc = *read_entries[i + j].tc;
                        ChatMessage call_msg;
                        call_msg.role = "tool_call";
                        call_msg.content = "[Tool: " + tc.function_name + "] " + tc.function_arguments;
                        call_msg.display_override =
                            ToolExecutor::build_tool_call_preview(tc.function_name, tc.function_arguments);
                        callbacks_.on_tool_result(call_msg, tc.function_name, results[idx]);
                    }
                }

                i = batch_end;
            }
        }

        // Phase 2: Execute write tools sequentially (with permission checks)
        for (const auto& entry : write_entries) {
            if (abort_requested_) break;

            const auto& tc = *entry.tc;
            LOG_INFO("Tool call (write): " + tc.function_name + " id=" + tc.id);

            dispatch_message("tool_call",
                    "[Tool: " + tc.function_name + "] " + tc.function_arguments, true);

            std::string ctx_path, ctx_command;
            extract_context(tc, ctx_path, ctx_command);

            bool auto_allow = permissions_.should_auto_allow(tc.function_name, false, ctx_path, ctx_command);

            auto emit_tool_result_callback = [&](size_t idx) {
                if (!callbacks_.on_tool_result) return;
                ChatMessage call_msg;
                call_msg.role = "tool_call";
                call_msg.content = "[Tool: " + tc.function_name + "] " + tc.function_arguments;
                call_msg.display_override =
                    ToolExecutor::build_tool_call_preview(tc.function_name, tc.function_arguments);
                callbacks_.on_tool_result(call_msg, tc.function_name, results[idx]);
            };

            // Path safety validation
            if (!ctx_path.empty() && tc.function_name != "bash") {
                std::string path_error = path_validator_.validate(ctx_path);
                if (!path_error.empty()) {
                    LOG_WARN("Path validation failed: " + path_error);
                    results[entry.original_index] = ToolResult{"[Error] " + path_error, false};
                    result_ready[entry.original_index] = true;
                    dispatch_message("tool_result", results[entry.original_index].output, true);
                    emit_tool_result_callback(entry.original_index);
                    continue;
                }

                // Dangerous path: force confirmation even in Yolo mode (unless -dangerous)
                if (path_validator_.is_dangerous_path(ctx_path) && auto_allow && !permissions_.is_dangerous()) {
                    LOG_INFO("Dangerous path detected, forcing confirmation: " + ctx_path);
                    auto_allow = false;
                }
            }

            if (!auto_allow && (prompter_ || callbacks_.on_tool_confirm)) {
                // Section 7.6: 优先走 prompter_(daemon 异步路径);未注入时
                // 回落到 callbacks_.on_tool_confirm(TUI 同步路径)。
                PermissionResult perm = prompter_
                    ? prompter_->prompt(tc.function_name, tc.function_arguments, &abort_requested_)
                    : callbacks_.on_tool_confirm(tc.function_name, tc.function_arguments);
                if (perm == PermissionResult::Deny) {
                    results[entry.original_index] = ToolResult{"[User denied tool execution]", false};
                    result_ready[entry.original_index] = true;
                    dispatch_message("tool_result", "[User denied tool execution]", true);
                    emit_tool_result_callback(entry.original_index);
                    continue;
                }
                if (perm == PermissionResult::AlwaysAllow) {
                    permissions_.add_session_allow(tc.function_name);
                }
            }

            std::string exec_path, exec_cmd;
            extract_context(tc, exec_path, exec_cmd);

            // Build command preview: for bash use the command string, else tool name + primary arg.
            std::string cmd_preview;
            if (!exec_cmd.empty()) cmd_preview = exec_cmd;
            else if (!exec_path.empty()) cmd_preview = exec_path;
            else cmd_preview = tc.function_name;
            if (cmd_preview.size() > 60) cmd_preview = cmd_preview.substr(0, 57) + "...";

            // 提前算 display_override(对齐 TUI: bash → "bash  cmd",file_* → "file_*  path",
            // 其他工具返回空 → 浏览器侧 fallback 到 tool 名 + 参数预览)。
            std::string display_override =
                ToolExecutor::build_tool_call_preview(tc.function_name, tc.function_arguments);
            bool is_task_complete = (tc.function_name == "task_complete");

            // 工具计时(供 ToolEnd.elapsed_seconds 与 ToolUpdate 实时秒数)
            auto tool_start_tp = std::chrono::steady_clock::now();

            // §1.2.2: 推 ToolStart 事件给 daemon 路径。TUI 路径不依赖此事件
            // (它走 callbacks_.on_tool_progress_start),所以两路并行不冲突。
            {
                nlohmann::json args_payload;
                try { args_payload = nlohmann::json::parse(tc.function_arguments); }
                catch (...) { args_payload = tc.function_arguments; }
                events_.emit(SessionEventKind::ToolStart,
                    web::build_tool_start_payload(tc.function_name, args_payload,
                                                    cmd_preview, display_override,
                                                    is_task_complete));
            }

            // Shared progress state for this one tool call.
            // Accessed from the streaming callback which runs on the bash_tool's
            // polling loop (same worker thread), so no extra synchronisation
            // beyond the mutable lambda capture.
            struct ProgressState {
                std::string current_line;
                std::deque<std::string> tail_lines;
                int total_lines = 0;
                size_t total_bytes = 0;
            };
            auto prog = std::make_shared<ProgressState>();

            ToolContext tool_ctx;
            tool_ctx.cwd = cwd_;
            tool_ctx.abort_flag = &abort_requested_;
            if (session_manager_) {
                tool_ctx.track_file_write_before = [this](const std::string& path) {
                    if (session_manager_) {
                        session_manager_->track_file_write_before(path);
                    }
                };
            }

            // §1.1.3: 把 ask_user_question prompter 包成 ctx 回调。daemon 工厂版
            // AskUserQuestion 工具(create_ask_user_question_tool_async)读这个回调;
            // TUI 工厂版完全忽略它,继续走 TuiState overlay 流程。
            if (ask_prompter_) {
                AskUserQuestionPrompter* p = ask_prompter_;
                std::atomic<bool>* abort_flag_ptr = &abort_requested_;
                tool_ctx.ask_user_questions =
                    [p, abort_flag_ptr](const nlohmann::json& questions_payload) -> nlohmann::json {
                        AskUserQuestionResponse resp = p->prompt(questions_payload, abort_flag_ptr);
                        nlohmann::json out;
                        out["cancelled"] = resp.cancelled;
                        nlohmann::json arr = nlohmann::json::array();
                        for (const auto& a : resp.answers) {
                            nlohmann::json item;
                            item["question_id"] = a.question_id;
                            item["selected"]    = a.selected;
                            item["custom_text"] = a.custom_text;
                            arr.push_back(std::move(item));
                        }
                        out["answers"] = std::move(arr);
                        return out;
                    };
            }

            // §1.2.3: stream 回调同时驱动 TUI 进度回调 + 推 ToolUpdate 事件。
            // current_partial = current_line(未完整 line),tail_lines = 最近 5 行(由
            // feed_line_state 维护)。elapsed_seconds 实时算。
            auto stream_update_cb = callbacks_.on_tool_progress_update;
            EventDispatcher* events_ptr = &events_;
            std::string tool_name_copy = tc.function_name;
            tool_ctx.stream = [prog, stream_update_cb, events_ptr, tool_start_tp,
                                tool_name_copy](const std::string& chunk) {
                feed_line_state(chunk, prog->current_line, prog->tail_lines, prog->total_lines);
                prog->total_bytes += chunk.size();
                std::vector<std::string> snapshot(prog->tail_lines.begin(), prog->tail_lines.end());
                if (stream_update_cb) {
                    stream_update_cb(snapshot, prog->current_line, prog->total_bytes, prog->total_lines);
                }
                auto elapsed_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - tool_start_tp).count();
                events_ptr->emit(SessionEventKind::ToolUpdate,
                    web::build_tool_update_payload(tool_name_copy, snapshot,
                                                     prog->current_line,
                                                     prog->total_lines,
                                                     prog->total_bytes,
                                                     elapsed_ms / 1000.0));
            };

            // RAII guard: ensures on_tool_progress_end fires on any exit path.
            struct ProgressGuard {
                std::function<void()> end_cb;
                ~ProgressGuard() { if (end_cb) end_cb(); }
            };
            ProgressGuard guard;
            if (callbacks_.on_tool_progress_start) {
                callbacks_.on_tool_progress_start(tc.function_name, cmd_preview);
                guard.end_cb = callbacks_.on_tool_progress_end;
            }

            results[entry.original_index] = execute_single_tool(
                tc.function_name, tc.function_arguments, exec_path, tool_ctx);
            result_ready[entry.original_index] = true;

            // §1.2.4: 推 ToolEnd 事件。summary 来自 ToolResult::summary(opt-in 工具
            // 才填);失败时 output 字段携带前 20 行 stderr 供前端 dim 显示。
            {
                const ToolResult& r = results[entry.original_index];
                auto elapsed_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - tool_start_tp).count();
                std::string snippet;
                if (!r.success) {
                    int lines = 0;
                    for (char c : r.output) {
                        snippet.push_back(c);
                        if (c == '\n' && ++lines >= 20) break;
                    }
                }
                events_.emit(SessionEventKind::ToolEnd,
                    web::build_tool_end_payload(tc.function_name, r,
                                                  elapsed_ms / 1000.0, snippet));
            }

            dispatch_message("tool_result", results[entry.original_index].output, true);
            emit_tool_result_callback(entry.original_index);
        }

        // Phase 3: Record all results in original order
        for (size_t i = 0; i < accumulated.tool_calls.size(); ++i) {
            const auto& tc = accumulated.tool_calls[i];
            ChatMessage tool_msg;
            if (result_ready[i]) {
                tool_msg = ToolExecutor::format_tool_result(tc.id, results[i]);
                // restore-tool-calls-on-resume: 注入视觉字段到 metadata,
                // 让 --resume 能彩色还原 diff 与摘要(老 session 没有这两个键 → fold 降级)。
                if (results[i].summary.has_value()) {
                    tool_msg.metadata["tool_summary"] = encode_tool_summary(*results[i].summary);
                }
                if (results[i].hunks.has_value()) {
                    tool_msg.metadata["tool_hunks"] = encode_tool_hunks(*results[i].hunks);
                }
            } else {
                // Tool was skipped (abort)
                tool_msg = ToolExecutor::format_tool_result(tc.id,
                    ToolResult{"[Interrupted]", false});
                // 中断分支不带 summary/hunks,自然不写 metadata。
            }
            messages_.push_back(tool_msg);
            if (session_manager_) session_manager_->on_message(tool_msg);
        }

        // Terminator detection. The ONLY terminator tool is task_complete.
        // AskUserQuestion is NOT a terminator — it is a regular tool: the
        // model asks a multi-choice question, the user's selection is fed
        // back to the model as a tool_result, and the loop continues on the
        // next turn (the model then acts on the answer, often calling more
        // tools). Treating AskUserQuestion as a terminator would mean "every
        // time the model asks a clarifying question, abandon the task",
        // which is wrong.
        for (size_t i = 0; i < accumulated.tool_calls.size(); ++i) {
            const auto& tc = accumulated.tool_calls[i];
            if (tc.function_name == "task_complete" && result_ready[i] && results[i].success) {
                terminator_fired = true;
                terminator_reason = "task_complete";
                LOG_INFO("Terminator fired: task_complete");
                break;
            }
        }

        // Loop back to call the provider again with the tool results (unless
        // terminator_fired or the outer while-condition bails us out).
    }

    // Post-loop reason emission. The abort and consecutive-empty branches emit
    // their own messages inside the loop; only the max_iterations branch lands
    // here with work still conceptually pending.
    if (!abort_requested_ && !terminator_fired && total_iterations >= max_iter) {
        std::string stop_msg = "Agent loop stopped: reached max_iterations (" +
                               std::to_string(max_iter) + ")";
        LOG_WARN(stop_msg);
        dispatch_message("system", stop_msg, false);
    }

    if (callbacks_.on_busy_changed) {
        callbacks_.on_busy_changed(false);
    }
    busy_ = false;
    events_.emit(SessionEventKind::BusyChanged, nlohmann::json{{"busy", false}});
    events_.emit(SessionEventKind::Done, nlohmann::json::object());
}

void AgentLoop::run_shell(const std::string& command) {
    abort_requested_ = false;
    busy_ = true;

    LOG_WARN("user_initiated_shell: " + log_truncate(command, 200));

    if (callbacks_.on_busy_changed) {
        callbacks_.on_busy_changed(true);
    }

    // Surface the invocation in the TUI using the usual tool_call styling so
    // the user sees a clear "-> bash command" line followed by its result.
    nlohmann::json args = {{"command", command}};
    std::string args_json = args.dump();
    dispatch_message("tool_call", "[Tool: bash] " + args_json, true);

    ToolResult result{"[Error] bash tool not registered", false};
    if (tools_.has_tool("bash")) {
        // Same progress plumbing as the agent-driven bash path.
        std::string cmd_preview = command;
        if (cmd_preview.size() > 60) cmd_preview = cmd_preview.substr(0, 57) + "...";

        struct ProgressState {
            std::string current_line;
            std::deque<std::string> tail_lines;
            int total_lines = 0;
            size_t total_bytes = 0;
        };
        auto prog = std::make_shared<ProgressState>();

        ToolContext tool_ctx;
        tool_ctx.cwd = cwd_;
        tool_ctx.abort_flag = &abort_requested_;
        if (callbacks_.on_tool_progress_update) {
            auto update_cb = callbacks_.on_tool_progress_update;
            tool_ctx.stream = [prog, update_cb](const std::string& chunk) {
                feed_line_state(chunk, prog->current_line, prog->tail_lines, prog->total_lines);
                prog->total_bytes += chunk.size();
                std::vector<std::string> snapshot(prog->tail_lines.begin(), prog->tail_lines.end());
                update_cb(snapshot, prog->current_line, prog->total_bytes, prog->total_lines);
            };
        }

        struct ProgressGuard {
            std::function<void()> end_cb;
            ~ProgressGuard() { if (end_cb) end_cb(); }
        };
        ProgressGuard guard;
        if (callbacks_.on_tool_progress_start) {
            callbacks_.on_tool_progress_start("bash", cmd_preview);
            guard.end_cb = callbacks_.on_tool_progress_end;
        }

        try {
            result = tools_.execute("bash", args_json, tool_ctx);
        } catch (const std::exception& e) {
            LOG_ERROR(std::string("shell exec exception: ") + e.what());
            result = ToolResult{std::string("[Error] ") + e.what(), false};
        }
    } else {
        LOG_WARN("Shell mode invoked but `bash` tool is not registered");
    }

    // 用户主动 `!cmd` 的输出必须**全量显示**(不折叠、不摘要、不截断)—— 用户
    // 自己输入命令就是为了看完整结果,LLM 工具结果的"摘要 + Ctrl+E 展开"语义
    // 在这里不适用。所以使用一个独立的 TUI 伪角色 `user_shell_output`,渲染分支
    // 走全量路径,与 `tool_result`(LLM 工具结果)区分开。
    // 同样不调 callbacks_.on_tool_result —— 它会把 ToolResult.summary 回填到
    // TuiState::Message,导致渲染走 summary 单行;这正是要避免的。
    dispatch_message("user_shell_output", result.output, true);

    // Persist the two display-side messages so --resume can rehydrate both the
    // chat view and (via the recovery pass in main.cpp) the LLM messages_.
    // 落盘的 role 仍然是 "tool_result"(伪角色) —— resume 时由 main.cpp
    // 的 shell-mode 配对识别(`is_shell_user && next_is_result`)把它翻译为
    // "user_shell_output"。不写 metadata.tool_summary/tool_hunks,因为
    // user_shell_output 渲染分支不读这些字段,写了也是死字段。
    if (session_manager_) {
        ChatMessage user_msg;
        user_msg.role = "user";
        user_msg.content = "!" + command;
        session_manager_->on_message(user_msg);

        ChatMessage tool_msg;
        tool_msg.role = "tool_result";
        tool_msg.content = result.output;
        session_manager_->on_message(tool_msg);
    }

    // Inject into LLM context for subsequent turns. BashTool currently merges
    // stdout+stderr into `result.output`, so we report it as stdout and leave
    // stderr empty; exit code derives from `success`.
    inject_shell_turn(command, result.output, "", result.success ? 0 : 1);

    if (callbacks_.on_busy_changed) {
        callbacks_.on_busy_changed(false);
    }
    busy_ = false;
    events_.emit(SessionEventKind::BusyChanged, nlohmann::json{{"busy", false}});
    events_.emit(SessionEventKind::Done, nlohmann::json::object());
}

} // namespace acecode
