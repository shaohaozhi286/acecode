#include "spawn_subagent_tool.hpp"

#include "../config/config.hpp"
#include "../session/session_client.hpp"
#include "../session/session_registry.hpp"
#include "../skills/skill_init.hpp"
#include "../skills/skill_registry.hpp"
#include "../utils/logger.hpp"
#include "../web/handlers/skill_command_expander.hpp"

#include <chrono>
#include <thread>

namespace acecode {

namespace {

using nlohmann::json;

ToolResult error_result(const std::string& message) {
    ToolResult r;
    r.success = false;
    r.output = message;
    return r;
}

// 从子会话的持久化消息里取最后一条非空 assistant 答复。
std::string last_assistant_text(SessionManager& sm) {
    auto messages = sm.load_active_messages();
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (it->role == "assistant" && !it->content.empty()) return it->content;
    }
    return {};
}

enum class WaitKind { Completed, Aborted, Timeout, Gone };

struct WaitOutcome {
    WaitKind    kind = WaitKind::Completed;
    std::string final_text;
};

// 轮询等待子会话本轮结束。
//
// 完成判定的两个细节:
//   - send_input 是异步入队,worker 线程稍后才置 busy —— 只看 !is_busy()
//     会在"还没开始"时误判完成。观察到过 busy(observed_busy)后回落才算;
//     兜底:2 秒内从未观测到 busy(极快 turn 在轮询间隙内完成)时,以
//     "出现了新的 assistant 消息"为完成信号。
//   - 父会话 abort(用户 Esc / 停止按钮)→ 传播给子会话 loop->abort(),
//     但**不**销毁子会话:已产生的工作保留在侧栏里,用户可接管。
WaitOutcome wait_for_subagent(SessionRegistry& registry,
                              const std::string& session_id,
                              const std::atomic<bool>* abort_flag,
                              int timeout_seconds,
                              std::size_t baseline_message_count) {
    using clock = std::chrono::steady_clock;
    const auto started = clock::now();
    bool observed_busy = false;

    while (true) {
        if (abort_flag && abort_flag->load()) {
            if (auto entry = registry.acquire(session_id); entry && entry->loop) {
                entry->loop->abort();
            }
            return {WaitKind::Aborted, {}};
        }

        auto entry = registry.acquire(session_id);
        if (!entry || !entry->loop || !entry->sm) return {WaitKind::Gone, {}};

        const bool busy = entry->loop->is_busy();
        if (busy) observed_busy = true;

        const auto elapsed = clock::now() - started;
        if (!busy) {
            const bool grace_passed = elapsed >= std::chrono::seconds(2);
            if (observed_busy) {
                // 曾观测到运行、现已空闲 → 本轮结束。final_text 可能为空
                // (turn 失败或被 UI 中止),由调用方给出对应文案,不死等。
                return {WaitKind::Completed, last_assistant_text(*entry->sm)};
            }
            if (grace_passed) {
                // 从未观测到 busy:要么 turn 快到在轮询间隙内完成(有新消息),
                // 要么 submit 后压根没跑起来。有新消息 → 完成;持续无消息
                // 超过 60 秒 → 放弃等待(不销毁子会话)。
                if (entry->sm->load_active_messages().size() > baseline_message_count) {
                    return {WaitKind::Completed, last_assistant_text(*entry->sm)};
                }
                if (elapsed >= std::chrono::seconds(60)) {
                    return {WaitKind::Timeout, {}};
                }
            }
        }

        if (timeout_seconds > 0 &&
            elapsed >= std::chrono::seconds(timeout_seconds)) {
            return {WaitKind::Timeout, {}};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

// prompt 以 '/' 开头时按子会话 cwd 做 skill 命令展开(与 Web 输入框 /
// POST messages 同一套 try_expand_skill_command 语义)。
void expand_skill_prompt(const SubagentToolDeps& deps,
                         const std::string& cwd,
                         std::string& prompt,
                         std::string& display_text) {
    if (prompt.empty() || prompt[0] != '/' || !deps.config) return;
    SkillRegistry tmp;
    initialize_skill_registry(tmp, *deps.config, cwd);
    auto expansion = web::try_expand_skill_command(prompt, tmp);
    if (expansion.expanded) {
        display_text = prompt;
        prompt = std::move(expansion.text);
    }
}

int parse_timeout_seconds(const json& args) {
    if (!args.contains("timeout_seconds")) return 0;
    if (args["timeout_seconds"].is_number_integer()) {
        int v = args["timeout_seconds"].get<int>();
        return v > 0 ? v : 0;
    }
    return 0;
}

ToolResult describe_wait_outcome(const WaitOutcome& outcome,
                                 const std::string& session_id) {
    ToolResult r;
    r.metadata["subagent_session_id"] = session_id;
    switch (outcome.kind) {
        case WaitKind::Completed:
            r.success = true;
            if (outcome.final_text.empty()) {
                r.output = "[subagent " + session_id +
                           "] finished without a final assistant reply "
                           "(possibly aborted from the UI).";
            } else {
                r.output = "[subagent " + session_id + " completed]\n\n" +
                           outcome.final_text;
            }
            return r;
        case WaitKind::Aborted:
            r.success = false;
            r.output = "[subagent " + session_id +
                       "] wait aborted; the subagent session was asked to stop "
                       "but is preserved in the session list.";
            return r;
        case WaitKind::Timeout:
            r.success = false;
            r.output = "[subagent " + session_id +
                       "] still running after the timeout. It keeps running in "
                       "the background; call wait_subagent later with this "
                       "session_id to collect the result.";
            return r;
        case WaitKind::Gone:
        default:
            r.success = false;
            r.output = "[subagent " + session_id +
                       "] session no longer exists (destroyed).";
            return r;
    }
}

} // namespace

ToolImpl create_spawn_subagent_tool(std::shared_ptr<SubagentToolDeps> deps) {
    ToolImpl tool;
    tool.definition.name = "spawn_subagent";
    tool.definition.description =
        "Start a sub-agent in a NEW isolated session (its own context window, "
        "shown in this session's background-tasks panel). Use it to delegate a "
        "self-contained task without polluting the current context, or to kick "
        "off the next stage of a pipeline. The prompt may be a skill command like "
        "'/my-skill args'. With wait=true (default) this blocks until the "
        "sub-agent finishes its turn and returns its final reply; with "
        "wait=false it returns immediately with the new session_id "
        "(fire-and-forget; combine with wait_subagent to join later). "
        "Sub-agents cannot spawn further sub-agents.";
    tool.definition.parameters = json{
        {"type", "object"},
        {"properties", json{
            {"prompt", json{
                {"type", "string"},
                {"description", "First user message for the sub-agent session. "
                                "May be a '/skill-name args' command."}}},
            {"wait", json{
                {"type", "boolean"},
                {"description", "true (default): block until the sub-agent "
                                "finishes and return its final reply. false: "
                                "return immediately with the session_id."}}},
            {"model", json{
                {"type", "string"},
                {"description", "Optional saved model name for the sub-agent "
                                "session (defaults to the daemon default)."}}},
            {"timeout_seconds", json{
                {"type", "integer"},
                {"description", "Optional wait timeout. 0 or omitted = wait "
                                "indefinitely (parent abort still cancels)."}}},
        }},
        {"required", json::array({"prompt"})},
    };
    // spawn 本身不触碰文件系统;子会话内部的危险操作由子会话自己的
    // PermissionManager 把关(权限模式继承父会话)。所以这里自动放行,
    // 避免流水线每次接力都弹确认。
    tool.is_read_only = true;

    tool.execute = [deps](const std::string& arguments_json,
                          const ToolContext& ctx) -> ToolResult {
        if (!deps || !deps->registry || !deps->client) {
            return error_result("spawn_subagent is only available in daemon mode.");
        }

        json args;
        try {
            args = json::parse(arguments_json);
        } catch (const std::exception& e) {
            return error_result(std::string("invalid arguments: ") + e.what());
        }
        const std::string prompt =
            args.contains("prompt") && args["prompt"].is_string()
                ? args["prompt"].get<std::string>() : std::string{};
        if (prompt.empty()) return error_result("prompt is required");
        const bool wait =
            !args.contains("wait") || !args["wait"].is_boolean() ||
            args["wait"].get<bool>();
        const std::string model_name =
            args.contains("model") && args["model"].is_string()
                ? args["model"].get<std::string>() : std::string{};
        const int timeout_seconds = parse_timeout_seconds(args);

        // 深度限制:子代理不能再派生。父会话 id 从注入的 SessionManager 拿。
        std::string parent_id;
        if (ctx.session_manager) parent_id = ctx.session_manager->current_session_id();
        std::string parent_permission_mode;
        bool parent_in_registry = false;
        if (!parent_id.empty()) {
            if (auto parent = deps->registry->acquire(parent_id)) {
                parent_in_registry = true;
                if (parent->subagent_depth >= 1) {
                    return error_result(
                        "sub-agents cannot spawn further sub-agents");
                }
                if (parent->perm) {
                    parent_permission_mode =
                        PermissionManager::mode_name(parent->perm->mode());
                }
            }
        }
        if (!parent_in_registry && deps->fallback_permissions) {
            // TUI 主会话不在 registry 里:权限模式从进程级 PermissionManager
            // 继承(与 daemon 的父会话继承语义一致)。
            parent_permission_mode = PermissionManager::mode_name(
                deps->fallback_permissions->mode());
        }

        SessionOptions opts;
        opts.cwd = ctx.cwd;
        opts.model_name = model_name;
        opts.permission_mode = parent_permission_mode;
        opts.subagent_depth = 1;
        // 父会话 id 持久化到子会话 meta:子会话从常规列表隐藏,归入父会话
        // 的「后台任务」面板;daemon 重启后依然识别为后台任务。
        opts.parent_session_id = parent_id;

        std::string child_id;
        try {
            child_id = deps->registry->create(opts);
        } catch (const std::exception& e) {
            return error_result(std::string("failed to create subagent session: ") +
                                e.what());
        }
        if (child_id.empty()) {
            return error_result("failed to create subagent session");
        }

        std::size_t baseline = 0;
        if (auto child = deps->registry->acquire(child_id); child && child->sm) {
            baseline = child->sm->load_active_messages().size();
        }

        std::string send_text = prompt;
        std::string display_text;
        expand_skill_prompt(*deps, ctx.cwd, send_text, display_text);
        if (!deps->client->send_input(child_id, send_text, display_text)) {
            return error_result("failed to send prompt to subagent session " +
                                child_id);
        }
        LOG_INFO("[subagent] spawned session " + child_id + " (wait=" +
                 (wait ? std::string("true") : std::string("false")) + ")");
        if (deps->on_spawn) {
            deps->on_spawn(child_id, prompt);
        }

        if (!wait) {
            ToolResult r;
            r.success = true;
            r.metadata["subagent_session_id"] = child_id;
            r.output = "Subagent session started: " + child_id +
                       "\nIt runs in its own isolated session (shown in the "
                       "background-tasks panel). Use wait_subagent with this "
                       "session_id if you need its result later.";
            return r;
        }

        auto outcome = wait_for_subagent(*deps->registry, child_id,
                                         ctx.abort_flag, timeout_seconds,
                                         baseline);
        return describe_wait_outcome(outcome, child_id);
    };
    return tool;
}

ToolImpl create_wait_subagent_tool(std::shared_ptr<SubagentToolDeps> deps) {
    ToolImpl tool;
    tool.definition.name = "wait_subagent";
    tool.definition.description =
        "Wait for a sub-agent session (started earlier with spawn_subagent "
        "wait=false) to finish its current turn, then return its latest reply. "
        "Use after fanning out several sub-agents in parallel.";
    tool.definition.parameters = json{
        {"type", "object"},
        {"properties", json{
            {"session_id", json{
                {"type", "string"},
                {"description", "The subagent session id returned by spawn_subagent."}}},
            {"timeout_seconds", json{
                {"type", "integer"},
                {"description", "Optional timeout. 0 or omitted = wait indefinitely."}}},
        }},
        {"required", json::array({"session_id"})},
    };
    tool.is_read_only = true;

    tool.execute = [deps](const std::string& arguments_json,
                          const ToolContext& ctx) -> ToolResult {
        if (!deps || !deps->registry) {
            return error_result("wait_subagent is only available in daemon mode.");
        }
        json args;
        try {
            args = json::parse(arguments_json);
        } catch (const std::exception& e) {
            return error_result(std::string("invalid arguments: ") + e.what());
        }
        const std::string session_id =
            args.contains("session_id") && args["session_id"].is_string()
                ? args["session_id"].get<std::string>() : std::string{};
        if (session_id.empty()) return error_result("session_id is required");
        if (!deps->registry->acquire(session_id)) {
            return error_result("unknown session: " + session_id);
        }
        const int timeout_seconds = parse_timeout_seconds(args);

        // baseline=0:wait_subagent 语义是"取最新答复",只要存在 assistant
        // 消息即可返回,不要求本次调用之后新产生。
        auto outcome = wait_for_subagent(*deps->registry, session_id,
                                         ctx.abort_flag, timeout_seconds,
                                         /*baseline_message_count=*/0);
        return describe_wait_outcome(outcome, session_id);
    };
    return tool;
}

} // namespace acecode
