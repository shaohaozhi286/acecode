#include "agent_loop.hpp"
#include "prompt/system_prompt.hpp"
#include "utils/encoding.hpp"
#include "utils/logger.hpp"
#include "utils/stream_processing.hpp"
#include "commands/compact.hpp"
#include "commands/micro_compact.hpp"
#include "session/tool_metadata_codec.hpp"
#include "session/session_rewind.hpp"
#include "session/thread_goal_store.hpp"
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
#include <cstdint>
#include <limits>

namespace acecode {

namespace {

std::int64_t now_epoch_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

nlohmann::json build_agent_progress_payload(
    const std::string& phase,
    const std::string& label,
    const std::string& detail,
    const std::string& tool,
    const std::string& tool_call_id,
    int tool_index,
    std::int64_t started_at_ms) {
    nlohmann::json payload;
    payload["phase"] = phase;
    payload["label"] = label;
    if (!detail.empty()) payload["detail"] = detail;
    if (!tool.empty()) payload["tool"] = tool;
    if (!tool_call_id.empty()) payload["tool_call_id"] = tool_call_id;
    if (tool_index >= 0) payload["tool_index"] = tool_index;
    if (started_at_ms > 0) payload["started_at_ms"] = started_at_ms;
    return payload;
}

std::string format_bytes_detail(std::size_t bytes) {
    if (bytes < 1024) return "参数 " + std::to_string(bytes) + " 字节";
    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss.precision(1);
    oss << "参数 " << (static_cast<double>(bytes) / 1024.0) << " KB";
    return oss.str();
}

bool is_hidden_goal_context_message(const ChatMessage& msg) {
    return msg.metadata.is_object() &&
           msg.metadata.value("hidden_goal_context", false);
}

std::string format_goal_status_chip(const ThreadGoal& goal) {
    std::ostringstream oss;
    oss << "goal: " << to_string(goal.status) << " "
        << TokenTracker::format_tokens(static_cast<int>(std::min<std::int64_t>(
               goal.tokens_used,
               static_cast<std::int64_t>(std::numeric_limits<int>::max()))));
    if (goal.token_budget.has_value()) {
        oss << "/" << TokenTracker::format_tokens(static_cast<int>(std::min<std::int64_t>(
            *goal.token_budget,
            static_cast<std::int64_t>(std::numeric_limits<int>::max()))));
    }
    return oss.str();
}

nlohmann::json build_transcript_replace_payload(
    const std::vector<ChatMessage>& messages,
    const CompactResult& result) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& msg : messages) {
        arr.push_back(web::chat_message_to_payload_json(msg));
    }
    return nlohmann::json{
        {"messages", std::move(arr)},
        {"messages_compressed", result.messages_compressed},
        {"estimated_tokens_saved", result.estimated_tokens_saved},
    };
}

} // namespace

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
            run_agent_with_display(task.payload, task.display_text, task.hidden_goal_context);
            break;
        case WorkerTask::Kind::Shell:
            run_shell(task.payload);
            break;
        case WorkerTask::Kind::Compact:
            run_compact();
            break;
        }
    }
}

void AgentLoop::submit(const std::string& user_message) {
    submit(user_message, std::string{});
}

void AgentLoop::submit(const std::string& prompt, const std::string& display_text) {
    {
        std::lock_guard<std::mutex> lk(queue_mu_);
        WorkerTask task;
        task.kind = WorkerTask::Kind::Chat;
        task.payload = prompt;
        task.display_text = display_text;
        task.hidden_goal_context = false;
        task_queue_.push(std::move(task));
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

void AgentLoop::submit_compact() {
    {
        std::lock_guard<std::mutex> lk(queue_mu_);
        WorkerTask task;
        task.kind = WorkerTask::Kind::Compact;
        task_queue_.push(std::move(task));
    }
    queue_cv_.notify_one();
}

void AgentLoop::emit_system_message(const std::string& content) {
    dispatch_message("system", content, false);
}

bool AgentLoop::active_estimate_exceeds_auto_threshold() const {
    auto [start, count] = get_messages_after_compact_boundary(messages_);
    std::vector<ChatMessage> active(messages_.begin() + start,
                                    messages_.begin() + start + count);
    return estimate_message_tokens(active) > get_auto_compact_threshold(context_window_);
}

void AgentLoop::apply_compact_result(const CompactResult& result) {
    messages_ = result.compacted_messages;
    last_api_prompt_tokens_.store(0);
    if (session_manager_) {
        session_manager_->replace_active_messages(messages_);
    }

    events_.emit(SessionEventKind::TranscriptReplace,
                 build_transcript_replace_payload(messages_, result));
    if (callbacks_.on_transcript_replace) {
        callbacks_.on_transcript_replace(messages_, result);
    }
}

bool AgentLoop::maybe_run_auto_compact() {
    if (auto_compact_consecutive_failures_ >= MAX_CONSECUTIVE_AUTOCOMPACT_FAILURES) {
        LOG_WARN("Auto-compact circuit breaker tripped (" +
                 std::to_string(auto_compact_consecutive_failures_) +
                 " consecutive failures)");
        dispatch_message("system",
                         "[Auto-compact] Skipped after repeated compaction failures.",
                         false);
        return false;
    }

    auto [boundary_start, boundary_count] = get_messages_after_compact_boundary(messages_);
    (void)boundary_count;
    int pre_tokens = estimate_message_tokens(
        std::vector<ChatMessage>(messages_.begin() + boundary_start, messages_.end()));

    auto micro_result = run_micro_compact(messages_, boundary_start);
    if (micro_result.performed) {
        messages_.push_back(create_microcompact_boundary_message(
            pre_tokens,
            micro_result.estimated_tokens_saved,
            micro_result.cleared_tool_call_ids));
        last_api_prompt_tokens_.store(0);
        if (session_manager_) {
            session_manager_->replace_active_messages(messages_);
        }

        CompactResult replace_result;
        replace_result.performed = true;
        replace_result.estimated_tokens_saved = micro_result.estimated_tokens_saved;
        replace_result.compacted_messages = messages_;
        events_.emit(SessionEventKind::TranscriptReplace,
                     build_transcript_replace_payload(messages_, replace_result));

        dispatch_message(
            "system",
            "[Micro-compact] Cleared " +
                std::to_string(micro_result.tool_results_cleared) +
                " old tool results, saved ~" +
                TokenTracker::format_tokens(micro_result.estimated_tokens_saved) +
                " tokens",
            false);

        if (!active_estimate_exceeds_auto_threshold()) {
            auto_compact_consecutive_failures_ = 0;
            return true;
        }
    }

    events_.emit(SessionEventKind::AgentProgress, nlohmann::json{
        {"phase", "compacting"},
        {"label", "Compacting conversation"},
        {"started_at_ms", now_epoch_ms()},
    });
    dispatch_message("system",
                     "[Auto-compact] Context approaching limit, compacting...",
                     false);

    std::shared_ptr<LlmProvider> provider_snapshot;
    if (provider_accessor_) provider_snapshot = provider_accessor_();
    if (!provider_snapshot) {
        auto_compact_consecutive_failures_++;
        dispatch_message("system",
                         "[Auto-compact] provider unavailable for compaction",
                         false);
        return false;
    }

    CompactResult result = compact_messages(
        *provider_snapshot,
        messages_,
        cwd_,
        4,
        true,
        &abort_requested_);

    if (!result.performed) {
        auto_compact_consecutive_failures_++;
        dispatch_message("system", "[Auto-compact] " + result.error, false);
        return false;
    }

    apply_compact_result(result);
    auto_compact_consecutive_failures_ = 0;

    dispatch_message(
        "system",
        "[Auto-compact] Compacted " + std::to_string(result.messages_compressed) +
            " messages, saved ~" +
            TokenTracker::format_tokens(result.estimated_tokens_saved) + " tokens",
        false);
    return true;
}

void AgentLoop::restore_goal_runtime() {
    goal_accounting_thread_id_.clear();
    goal_accounting_goal_id_.clear();
    goal_time_checkpoint_ = {};
    if (!session_manager_) return;

    const std::string sid = session_manager_->current_session_id();
    ThreadGoalStore* store = session_manager_->existing_goal_store();
    if (!store || sid.empty()) return;

    std::string error;
    auto goal = store->get_thread_goal(sid, &error);
    if (!error.empty()) {
        LOG_WARN("[goal] failed to restore runtime state: " + error);
        return;
    }
    if (!goal.has_value() || goal->status != ThreadGoalStatus::Active) return;
    goal_accounting_thread_id_ = sid;
    goal_accounting_goal_id_ = goal->goal_id;
    goal_time_checkpoint_ = std::chrono::steady_clock::now();
}

void AgentLoop::publish_current_goal_state() {
    if (!session_manager_) {
        if (callbacks_.on_goal_status) callbacks_.on_goal_status(std::string{});
        return;
    }

    const std::string sid = session_manager_->current_session_id();
    if (sid.empty()) {
        if (callbacks_.on_goal_status) callbacks_.on_goal_status(std::string{});
        return;
    }

    ThreadGoalStore* store = session_manager_->existing_goal_store();
    if (!store) {
        emit_goal_cleared(sid);
        return;
    }

    std::string error;
    auto goal = store->get_thread_goal(sid, &error);
    if (!error.empty()) {
        LOG_WARN("[goal] failed to publish current goal state: " + error);
        if (callbacks_.on_goal_status) callbacks_.on_goal_status(std::string{});
        return;
    }
    if (goal.has_value()) {
        emit_goal_updated(*goal);
    } else {
        emit_goal_cleared(sid);
    }
}

void AgentLoop::emit_goal_updated(const ThreadGoal& goal) {
    events_.emit(SessionEventKind::GoalUpdated,
        nlohmann::json{{"session_id", goal.thread_id}, {"goal", thread_goal_to_json(goal)}});
    if (callbacks_.on_goal_status) {
        callbacks_.on_goal_status(format_goal_status_chip(goal));
    }
    if (goal.status == ThreadGoalStatus::Active) {
        goal_accounting_thread_id_ = goal.thread_id;
        goal_accounting_goal_id_ = goal.goal_id;
        goal_time_checkpoint_ = std::chrono::steady_clock::now();
    } else if (goal.goal_id == goal_accounting_goal_id_) {
        goal_accounting_thread_id_.clear();
        goal_accounting_goal_id_.clear();
        goal_time_checkpoint_ = {};
    }
}

void AgentLoop::emit_goal_cleared(const std::string& session_id) {
    events_.emit(SessionEventKind::GoalCleared,
        nlohmann::json{{"session_id", session_id}});
    if (callbacks_.on_goal_status) callbacks_.on_goal_status(std::string{});
    if (session_id == goal_accounting_thread_id_) {
        goal_accounting_thread_id_.clear();
        goal_accounting_goal_id_.clear();
        goal_time_checkpoint_ = {};
    }
}

void AgentLoop::account_goal_usage(std::int64_t token_delta, bool allow_complete) {
    if (!session_manager_) return;
    const std::string sid = session_manager_->current_session_id();
    ThreadGoalStore* store = session_manager_->existing_goal_store();
    if (!store || sid.empty()) return;

    if (goal_accounting_thread_id_ != sid || goal_accounting_goal_id_.empty()) {
        restore_goal_runtime();
    }
    if (goal_accounting_thread_id_ != sid || goal_accounting_goal_id_.empty()) return;

    const auto now = std::chrono::steady_clock::now();
    std::int64_t elapsed_seconds = 0;
    if (goal_time_checkpoint_.time_since_epoch().count() != 0) {
        elapsed_seconds = std::chrono::duration_cast<std::chrono::seconds>(
            now - goal_time_checkpoint_).count();
    }
    goal_time_checkpoint_ = now;

    std::string error;
    auto result = store->account_thread_goal_usage(
        sid,
        goal_accounting_goal_id_,
        std::max<std::int64_t>(0, token_delta),
        elapsed_seconds,
        allow_complete,
        &error);
    if (!error.empty()) {
        LOG_WARN("[goal] accounting failed: " + error);
        return;
    }
    if (!result.goal.has_value()) return;

    if (result.became_budget_limited) {
        emit_goal_updated(*result.goal);
        if (budget_notice_goal_id_ != result.goal->goal_id) {
            budget_notice_goal_id_ = result.goal->goal_id;
            dispatch_message("system", "[Goal] Token budget reached; automatic continuation stopped.", false);
        }
        return;
    }

    if (result.updated) {
        emit_goal_updated(*result.goal);
    }
}

std::string AgentLoop::build_goal_context_prompt(const ThreadGoal& goal) const {
    std::ostringstream oss;
    oss << "<goal_context>\n"
        << "Continue working toward the active thread goal.\n\n"
        << "Objective:\n"
        << goal.objective << "\n\n"
        << "Status: " << to_string(goal.status) << "\n"
        << "Tokens used: " << goal.tokens_used;
    if (goal.token_budget.has_value()) {
        oss << " / " << *goal.token_budget
            << " (remaining "
            << std::max<std::int64_t>(0, *goal.token_budget - goal.tokens_used)
            << ")";
    }
    oss << "\nElapsed seconds: " << goal.time_used_seconds << "\n\n"
        << "When the objective is actually achieved, call update_goal with "
        << "status \"complete\". Do not mark it complete for partial progress.\n"
        << "</goal_context>";
    return oss.str();
}

void AgentLoop::maybe_continue_goal() {
    if (!session_manager_ || abort_requested_.load() || busy_.load()) return;
    const std::string sid = session_manager_->current_session_id();
    ThreadGoalStore* store = session_manager_->existing_goal_store();
    if (!store || sid.empty()) return;

    std::string error;
    auto goal = store->get_thread_goal(sid, &error);
    if (!error.empty()) {
        LOG_WARN("[goal] failed to load goal for continuation: " + error);
        return;
    }
    if (!goal.has_value() || goal->status != ThreadGoalStatus::Active) return;

    {
        std::lock_guard<std::mutex> lk(queue_mu_);
        if (shutdown_requested_ || !task_queue_.empty()) return;
        WorkerTask task;
        task.kind = WorkerTask::Kind::Chat;
        task.payload = build_goal_context_prompt(*goal);
        task.hidden_goal_context = true;
        task_queue_.push(std::move(task));
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
    run_agent_with_display(user_message, std::string{}, false);
}

void AgentLoop::run_agent_with_display(const std::string& user_message,
                                        const std::string& display_text,
                                        bool hidden_goal_context) {
    abort_requested_ = false;
    busy_ = true;
    restore_goal_runtime();

    LOG_INFO("=== submit() user_message: " + log_truncate(user_message, 200));

    // Add user message
    ChatMessage user_msg;
    user_msg.role = "user";
    user_msg.content = user_message;
    if (!display_text.empty() && display_text != user_message) {
        // 让 UI 渲染 display_text(原文),LLM 看到的仍是 user_message(展开后的)。
        // session_serializer 会把 metadata 全字段持久化,resume 后恢复。
        if (!user_msg.metadata.is_object()) user_msg.metadata = nlohmann::json::object();
        user_msg.metadata["display_text"] = display_text;
    }
    if (hidden_goal_context) {
        if (!user_msg.metadata.is_object()) user_msg.metadata = nlohmann::json::object();
        user_msg.metadata["hidden_goal_context"] = true;
    }
    ensure_user_message_identity(user_msg);
    messages_.push_back(user_msg);
    if (session_manager_) {
        session_manager_->on_message(user_msg);
        if (!hidden_goal_context) {
            session_manager_->begin_user_turn_checkpoint(user_msg.uuid);
        }
    }
    if (!hidden_goal_context) {
        nlohmann::json msg_event = {
            {"role", "user"}, {"content", user_message},
            {"is_tool", false}, {"id", user_msg.uuid},
        };
        if (!user_msg.metadata.is_null() && !user_msg.metadata.empty()) {
            msg_event["metadata"] = user_msg.metadata;
        }
        events_.emit(SessionEventKind::Message, msg_event);
    }

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

    std::mutex progress_mu;
    std::string active_progress_key;
    std::int64_t active_progress_started_at_ms = 0;
    std::chrono::steady_clock::time_point last_progress_emit_at{};
    auto emit_agent_progress = [&](const std::string& phase,
                                   const std::string& label,
                                   const std::string& detail = std::string{},
                                   const std::string& tool = std::string{},
                                   const std::string& tool_call_id = std::string{},
                                   int tool_index = -1,
                                   bool force = false) {
        const auto now = std::chrono::steady_clock::now();
        const std::string key = phase + "\0" + tool + "\0" + tool_call_id + "\0" + std::to_string(tool_index);
        nlohmann::json payload;
        {
            std::lock_guard<std::mutex> lk(progress_mu);
            if (key != active_progress_key) {
                active_progress_key = key;
                active_progress_started_at_ms = now_epoch_ms();
                force = true;
            }
            if (!force && last_progress_emit_at.time_since_epoch().count() != 0) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_progress_emit_at);
                if (elapsed < std::chrono::milliseconds(750)) return;
            }
            last_progress_emit_at = now;
            payload = build_agent_progress_payload(
                phase, label, detail, tool, tool_call_id, tool_index,
                active_progress_started_at_ms);
        }
        EventDispatcher::EmitOptions opts;
        opts.buffered = true;
        opts.coalesce_key = "agent_progress";
        events_.emit(SessionEventKind::AgentProgress, std::move(payload), opts);
    };

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
            maybe_run_auto_compact();
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
        std::size_t reasoning_bytes = 0;
        int reasoning_fragments = 0;

        auto stream_callback = [&accumulated, &resp_mu, &emit_agent_progress,
                                &reasoning_bytes, &reasoning_fragments, this](const StreamEvent& evt) {
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
                reasoning_bytes += evt.content.size();
                reasoning_fragments++;
                emit_agent_progress("reasoning", "正在推理",
                    "片段 " + std::to_string(reasoning_fragments) + ", " +
                    std::to_string(reasoning_bytes) + " 字节");
                // Future TUI hook (e.g. a "Thinking..." panel) can subscribe
                // here. Today we only accumulate so format_assistant_tool_calls
                // and the empty-turn branch can echo it back to DeepSeek.
                events_.emit(SessionEventKind::Reasoning, nlohmann::json{{"text", evt.content}});
                break;
            case StreamEventType::ToolCallDelta:
                {
                    const std::string tool_name = evt.tool_call.function_name;
                    const std::string label = tool_name.empty()
                        ? "正在准备工具调用"
                        : "正在准备调用 " + tool_name;
                    emit_agent_progress("tool_planning", label,
                        format_bytes_detail(evt.tool_call_argument_bytes),
                        tool_name, evt.tool_call.id, evt.tool_index);
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
            case StreamEventType::Usage:
                last_api_prompt_tokens_.store(evt.usage.prompt_tokens);
                if (evt.usage.has_data) {
                    account_goal_usage(evt.usage.total_tokens, false);
                }
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
            emit_agent_progress(total_iterations == 1 ? "model_waiting" : "model_followup",
                total_iterations == 1 ? "正在等待模型响应" : "正在等待模型继续响应",
                std::string{}, std::string{}, std::string{}, -1, true);
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

        if (!accumulated.usage.has_data &&
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
            account_goal_usage(estimated_usage.total_tokens, false);
            if (callbacks_.on_usage) {
                callbacks_.on_usage(estimated_usage);
            }
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

        using ToolRunner = std::function<ToolResult(const ToolContext&,
                                                     const std::string&,
                                                     const std::string&)>;

        auto run_tool_with_lifecycle = [&](const ToolCall& tc,
                                           size_t tool_index,
                                           bool emit_tui_progress,
                                           const ToolRunner& runner) -> ToolResult {
            std::string exec_path, exec_cmd;
            extract_context(tc, exec_path, exec_cmd);

            std::string cmd_preview;
            if (!exec_cmd.empty()) cmd_preview = exec_cmd;
            else if (!exec_path.empty()) cmd_preview = exec_path;
            else cmd_preview = tc.function_name;
            cmd_preview = truncate_utf8_prefix(cmd_preview, 60);

            std::string display_override =
                ToolExecutor::build_tool_call_preview(tc.function_name, tc.function_arguments);
            bool is_task_complete = (tc.function_name == "task_complete");

            auto tool_start_tp = std::chrono::steady_clock::now();
            const int tool_index_int = static_cast<int>(tool_index);

            {
                nlohmann::json args_payload;
                try { args_payload = nlohmann::json::parse(tc.function_arguments); }
                catch (...) { args_payload = tc.function_arguments; }
                events_.emit(SessionEventKind::ToolStart,
                    web::build_tool_start_payload(tc.function_name, args_payload,
                                                    cmd_preview, display_override,
                                                    is_task_complete,
                                                    tc.id, tool_index_int));
            }

            emit_agent_progress("tool_running", "正在调用工具 " + tc.function_name,
                cmd_preview, tc.function_name, tc.id, tool_index_int, true);

            struct ProgressState {
                std::mutex mu;
                std::string current_line;
                std::deque<std::string> tail_lines;
                int total_lines = 0;
                size_t total_bytes = 0;
                std::chrono::steady_clock::time_point last_emit_at{};
            };
            auto prog = std::make_shared<ProgressState>();

            ToolContext tool_ctx;
            tool_ctx.cwd = cwd_;
            tool_ctx.abort_flag = &abort_requested_;
            tool_ctx.session_manager = session_manager_;
            tool_ctx.account_goal_usage = [this]() {
                account_goal_usage(0, true);
            };
            tool_ctx.emit_goal_updated = [this](const nlohmann::json& goal_payload) {
                if (session_manager_) {
                    const std::string sid = session_manager_->current_session_id();
                    ThreadGoalStore* store = session_manager_->existing_goal_store();
                    if (store && !sid.empty()) {
                        auto goal = store->get_thread_goal(sid);
                        if (goal.has_value()) {
                            emit_goal_updated(*goal);
                            return;
                        }
                    }
                    events_.emit(SessionEventKind::GoalUpdated,
                        nlohmann::json{{"session_id", sid}, {"goal", goal_payload}});
                }
            };
            tool_ctx.emit_goal_cleared = [this](const std::string& session_id) {
                emit_goal_cleared(session_id);
            };
            if (session_manager_) {
                tool_ctx.track_file_write_before = [this](const std::string& path) {
                    if (session_manager_) {
                        session_manager_->track_file_write_before(path);
                    }
                };
            }
            if (ask_prompter_) {
                AskUserQuestionPrompter* p = ask_prompter_;
                std::atomic<bool>* abort_flag_ptr = &abort_requested_;
                auto ask_progress = emit_agent_progress;
                const std::string tool_name_for_question = tc.function_name;
                const std::string tool_call_id_for_question = tc.id;
                tool_ctx.ask_user_questions =
                    [p, abort_flag_ptr, ask_progress, tool_name_for_question,
                     tool_call_id_for_question, tool_index_int](const nlohmann::json& questions_payload) -> nlohmann::json {
                        ask_progress("question_waiting", "正在等待用户回答",
                            std::string{}, tool_name_for_question,
                            tool_call_id_for_question, tool_index_int, true);
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

            std::function<void(const std::vector<std::string>&,
                               const std::string&,
                               size_t,
                               int)> stream_update_cb;
            if (emit_tui_progress) stream_update_cb = callbacks_.on_tool_progress_update;
            EventDispatcher* events_ptr = &events_;
            std::string tool_name_copy = tc.function_name;
            std::string tool_call_id_copy = tc.id;
            const std::string update_coalesce_key = "tool_update:" +
                (!tc.id.empty() ? tc.id : (tc.function_name + ":" + std::to_string(tool_index_int)));
            tool_ctx.stream = [prog, stream_update_cb, events_ptr, tool_start_tp,
                                tool_name_copy, tool_call_id_copy, tool_index_int,
                                update_coalesce_key](const std::string& chunk) {
                std::vector<std::string> snapshot;
                std::string current_partial;
                int total_lines = 0;
                size_t total_bytes = 0;
                bool should_emit = false;
                {
                    std::lock_guard<std::mutex> lk(prog->mu);
                    feed_line_state(chunk, prog->current_line, prog->tail_lines, prog->total_lines);
                    prog->total_bytes += chunk.size();
                    snapshot.assign(prog->tail_lines.begin(), prog->tail_lines.end());
                    current_partial = prog->current_line;
                    total_lines = prog->total_lines;
                    total_bytes = prog->total_bytes;
                    const auto now = std::chrono::steady_clock::now();
                    should_emit = prog->last_emit_at.time_since_epoch().count() == 0 ||
                        std::chrono::duration_cast<std::chrono::milliseconds>(now - prog->last_emit_at) >=
                            std::chrono::milliseconds(500);
                    if (should_emit) prog->last_emit_at = now;
                }
                if (stream_update_cb) {
                    stream_update_cb(snapshot, current_partial, total_bytes, total_lines);
                }
                if (!should_emit) return;
                auto elapsed_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - tool_start_tp).count();
                EventDispatcher::EmitOptions opts;
                opts.buffered = true;
                opts.coalesce_key = update_coalesce_key;
                events_ptr->emit(SessionEventKind::ToolUpdate,
                    web::build_tool_update_payload(tool_name_copy, snapshot,
                                                     current_partial,
                                                     total_lines,
                                                     total_bytes,
                                                     elapsed_ms / 1000.0,
                                                     tool_call_id_copy,
                                                     tool_index_int),
                    opts);
            };

            struct ProgressGuard {
                std::function<void()> end_cb;
                ~ProgressGuard() { if (end_cb) end_cb(); }
            };
            ProgressGuard guard;
            if (emit_tui_progress && callbacks_.on_tool_progress_start) {
                callbacks_.on_tool_progress_start(tc.function_name, cmd_preview);
                guard.end_cb = callbacks_.on_tool_progress_end;
            }

            ToolResult result;
            try {
                result = runner(tool_ctx, exec_path, exec_cmd);
            } catch (const std::exception& e) {
                LOG_ERROR("Tool lifecycle runner error: " + std::string(e.what()));
                result = ToolResult{"[Error] Tool execution failed: " + std::string(e.what()), false};
            }

            auto elapsed_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - tool_start_tp).count();
            std::string snippet;
            if (!result.success) {
                int lines = 0;
                for (char c : result.output) {
                    snippet.push_back(c);
                    if (c == '\n' && ++lines >= 20) break;
                }
            }
            events_.emit(SessionEventKind::ToolEnd,
                web::build_tool_end_payload(tc.function_name, result,
                                              elapsed_ms / 1000.0, snippet,
                                              tc.id, tool_index_int));
            return result;
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
                    ToolCall tc_copy = *entry.tc;
                    size_t original_index = entry.original_index;
                    futures.push_back(std::async(std::launch::async,
                        [&run_tool_with_lifecycle, &execute_single_tool,
                         tc_copy, original_index]() {
                            return run_tool_with_lifecycle(
                                tc_copy, original_index, false,
                                [&execute_single_tool, &tc_copy](const ToolContext& ctx,
                                                                  const std::string& ctx_path,
                                                                  const std::string&) {
                                    return execute_single_tool(
                                        tc_copy.function_name, tc_copy.function_arguments,
                                        ctx_path, ctx);
                                });
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
                    account_goal_usage(0, false);

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

            auto emit_tool_result_callback = [&](size_t idx) {
                if (!callbacks_.on_tool_result) return;
                ChatMessage call_msg;
                call_msg.role = "tool_call";
                call_msg.content = "[Tool: " + tc.function_name + "] " + tc.function_arguments;
                call_msg.display_override =
                    ToolExecutor::build_tool_call_preview(tc.function_name, tc.function_arguments);
                callbacks_.on_tool_result(call_msg, tc.function_name, results[idx]);
            };

            results[entry.original_index] = run_tool_with_lifecycle(
                tc, entry.original_index, true,
                [&](const ToolContext& tool_ctx,
                    const std::string& ctx_path,
                    const std::string& ctx_command) -> ToolResult {
                    bool auto_allow = permissions_.should_auto_allow(
                        tc.function_name, false, ctx_path, ctx_command);

                    if (!ctx_path.empty() && tc.function_name != "bash") {
                        std::string path_error = path_validator_.validate(ctx_path);
                        if (!path_error.empty()) {
                            LOG_WARN("Path validation failed: " + path_error);
                            return ToolResult{"[Error] " + path_error, false};
                        }
                        if (path_validator_.is_dangerous_path(ctx_path) && auto_allow &&
                            !permissions_.is_dangerous() &&
                            permissions_.mode() != PermissionMode::Yolo) {
                            LOG_INFO("Dangerous path detected, forcing confirmation: " + ctx_path);
                            auto_allow = false;
                        }
                    }

                    if (!auto_allow && (prompter_ || callbacks_.on_tool_confirm)) {
                        emit_agent_progress("permission_waiting", "正在等待权限确认",
                            tc.function_name, tc.function_name, tc.id,
                            static_cast<int>(entry.original_index), true);
                        PermissionResult perm = prompter_
                            ? prompter_->prompt(tc.function_name, tc.function_arguments, &abort_requested_)
                            : callbacks_.on_tool_confirm(tc.function_name, tc.function_arguments);
                        if (perm == PermissionResult::Deny) {
                            return ToolResult{"[User denied tool execution]", false};
                        }
                        if (perm == PermissionResult::AlwaysAllow) {
                            permissions_.add_session_allow(tc.function_name);
                        }
                    }

                    return execute_single_tool(tc.function_name, tc.function_arguments,
                                               ctx_path, tool_ctx);
                });
            result_ready[entry.original_index] = true;
            account_goal_usage(0, false);

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

    if (abort_requested_) {
        account_goal_usage(0, false);
        if (session_manager_) {
            const std::string sid = session_manager_->current_session_id();
            ThreadGoalStore* store = session_manager_->goal_store();
            if (store && !sid.empty()) {
                std::string error;
                if (store->pause_active_thread_goal(sid, &error)) {
                    auto goal = store->get_thread_goal(sid);
                    if (goal.has_value()) emit_goal_updated(*goal);
                }
            }
        }
    } else {
        account_goal_usage(0, false);
    }

    if (callbacks_.on_busy_changed) {
        callbacks_.on_busy_changed(false);
    }
    busy_ = false;
    events_.emit(SessionEventKind::BusyChanged, nlohmann::json{{"busy", false}});
    events_.emit(SessionEventKind::Done, nlohmann::json::object());
    maybe_continue_goal();
}

void AgentLoop::run_compact() {
    abort_requested_ = false;
    busy_ = true;

    if (callbacks_.on_busy_changed) {
        callbacks_.on_busy_changed(true);
    }
    events_.emit(SessionEventKind::BusyChanged, nlohmann::json{{"busy", true}});
    events_.emit(SessionEventKind::AgentProgress, nlohmann::json{
        {"phase", "compacting"},
        {"label", "Compacting conversation"},
        {"started_at_ms", now_epoch_ms()},
    });
    dispatch_message("system", "Compacting conversation...", false);

    auto finish = [this]() {
        if (callbacks_.on_busy_changed) {
            callbacks_.on_busy_changed(false);
        }
        busy_ = false;
        events_.emit(SessionEventKind::BusyChanged, nlohmann::json{{"busy", false}});
        events_.emit(SessionEventKind::Done, nlohmann::json::object());
    };

    std::shared_ptr<LlmProvider> provider_snapshot;
    if (provider_accessor_) provider_snapshot = provider_accessor_();
    if (!provider_snapshot) {
        dispatch_message("error", "[Error] provider unavailable for /compact", false);
        finish();
        return;
    }

    CompactResult result = compact_messages(
        *provider_snapshot,
        messages_,
        cwd_,
        4,
        false,
        &abort_requested_);

    if (!result.performed) {
        dispatch_message("error", "[Error] " + result.error, false);
        finish();
        return;
    }

    apply_compact_result(result);

    dispatch_message(
        "system",
        "Compacted " + std::to_string(result.messages_compressed) +
            " messages, saved ~" +
            std::to_string(result.estimated_tokens_saved) + " tokens.",
        false);
    finish();
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
        cmd_preview = truncate_utf8_prefix(cmd_preview, 60);

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
