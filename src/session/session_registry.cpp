#include "session_registry.hpp"

#include "compact_checkpoint.hpp"
#include "session_rewind.hpp"
#include "session_resume_restore.hpp"
#include "session_storage.hpp"
#include "session_auto_title.hpp"
#include "thread_goal_store.hpp"
#include "tool_result_storage.hpp"
#include "turn_timing.hpp"
#include "../commands/init_command.hpp"
#include "../commands/lsp_command.hpp"
#include "../provider/apply_model_to_session.hpp"
#include "../provider/copilot_provider.hpp"
#include "../provider/cwd_model_override.hpp"
#include "../provider/model_context_resolver.hpp"
#include "../provider/model_pool_status.hpp"
#include "../provider/model_resolver.hpp"
#include "../provider/provider_factory.hpp"
#include "../skills/skill_init.hpp"
#include "../gitinfo/git_context_core.hpp"
#include "../worktree/worktree_core.hpp"
#include "../worktree/worktree_manager.hpp"
#include "../utils/logger.hpp"
#include "../utils/cwd_hash.hpp"
#include "../utils/power_inhibitor.hpp"
#include "../utils/utf8_path.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>

namespace acecode {

namespace {

std::string session_dir_name_from_id(const std::string& session_id) {
    std::string out;
    out.reserve(session_id.size());
    for (unsigned char ch : session_id) {
        if (std::isalnum(ch) || ch == '-' || ch == '_') {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('_');
        }
    }
    return out.empty() ? "session" : out;
}

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

void append_model_messages_to_loop(AgentLoop& loop,
                                   const std::vector<ChatMessage>& messages) {
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
            loop.inject_shell_turn(msg.content.substr(1),
                                   messages[i + 1].content,
                                   "",
                                   0);
            ++i;
            continue;
        }

        if (is_llm_role(msg.role) && !is_transcript_only_message(msg)) {
            loop.push_message(msg);
        }
    }
}

std::pair<std::string, std::string>
current_provider_model(const SessionRegistryDeps& deps,
                       const std::string& fallback_model) {
    (void)fallback_model;
    if (deps.provider_accessor) {
        auto provider = deps.provider_accessor();
        if (provider) {
            return {provider->name(), provider->model()};
        }
    }
    return {"", ""};
}

const ModelProfile* find_profile_by_name(const AppConfig& cfg,
                                          const std::string& name) {
    if (name.empty()) return nullptr;
    for (const auto& entry : cfg.saved_models) {
        if (entry.name == name) return &entry;
    }
    return nullptr;
}

std::optional<ModelProfile> explicit_profile(const AppConfig& cfg,
                                             const std::string& name) {
    if (name.empty()) return std::nullopt;
    if (const auto* entry = find_profile_by_name(cfg, name)) {
        ModelProfile profile = *entry;
        if (profile.provider == "openai" && !profile.stream_timeout_ms.has_value()) {
            profile.stream_timeout_ms = cfg.openai.stream_timeout_ms;
        }
        return profile;
    }
    return std::nullopt;
}

SessionModelState state_from_profile(const AppConfig& cfg,
                                     const ModelProfile& profile) {
    SessionModelState state;
    state.name = profile.name;
    state.provider = profile.provider;
    state.model = profile.model;
    state.context_window = resolve_model_profile_context_window_nonblocking(
        cfg, profile, cfg.context_window);
    // PUB 池模型:用监控上报的 0.8 * maxWindowTokens 作为有效上下文窗口(驱动占用%
    // 与自动压缩)。监控尚无数据时 eff==0,保留上面解析出的默认值。
    if (is_pub_model(state.model)) {
        int eff = model_pool_status_service().effective_context_window_for(state.model);
        if (eff > 0) state.context_window = eff;
    }
    return state;
}

SessionModelState deleted_state_from_name(const std::string& name) {
    SessionModelState state;
    state.name = name;
    state.deleted = true;
    return state;
}

void mark_deleted_if_model_name_missing(const AppConfig& cfg, SessionModelState& state) {
    if (state.name.empty() || state.name.rfind("(session:", 0) == 0) return;
    if (find_profile_by_name(cfg, state.name) != nullptr) return;
    state.provider.clear();
    state.model.clear();
    state.context_window = 0;
    state.deleted = true;
}

struct ResolvedSessionModel {
    SessionModelState state;
    std::shared_ptr<LlmProvider> provider;
};

ResolvedSessionModel resolve_from_profile(const AppConfig& cfg,
                                          const ModelProfile& profile) {
    ResolvedSessionModel resolved;
    resolved.state = state_from_profile(cfg, profile);
    resolved.provider = create_provider_from_entry(profile, &cfg);
    LOG_INFO("[registry] resolve_from_profile name='" + profile.name +
             "' provider='" + profile.provider + "' model='" + profile.model + "'");
    // Copilot provider 持有 github_token_ + copilot_token_ 两层鉴权状态,
    // create_provider_from_entry 只是构造空实例 — 必须显式 silent_auth 加载磁盘
    // 上的 github_token,否则首次 chat() 直接报 "Copilot session token unavailable"。
    // 比对 provider name 兼容 ModelProfile 中 provider 字段为空 / 大小写差异的场景:
    // 优先用实际 provider 实例的 name() (来自 LlmProvider::name() override),
    // 这一定是 "copilot" 当且仅当工厂确实构造了 CopilotProvider。
    if (resolved.provider) {
        const std::string actual_name = resolved.provider->name();
        if (actual_name == "copilot") {
            if (auto copilot = std::dynamic_pointer_cast<CopilotProvider>(resolved.provider)) {
                LOG_INFO("[registry] running silent_auth for new Copilot provider instance");
                if (!copilot->try_silent_auth()) {
                    LOG_WARN("[registry] Copilot silent_auth failed for new session provider "
                             "(model='" + profile.model + "'); user will see "
                             "'session token unavailable' until re-authentication");
                } else {
                    LOG_INFO("[registry] Copilot silent_auth OK for new session provider");
                }
            }
        }
    }
    return resolved;
}

ResolvedSessionModel resolve_session_model(const SessionRegistryDeps& deps,
                                           const SessionOptions& opts,
                                           const SessionMeta* resumed_meta) {
    if (deps.config) {
        ModelProfile profile;
        std::optional<std::string> cwd_override;
        if (!opts.cwd.empty()) {
            cwd_override = load_cwd_model_override(opts.cwd);
        }
        if (!opts.model_name.empty()) {
            auto explicit_match = explicit_profile(*deps.config, opts.model_name);
            if (explicit_match.has_value()) {
                profile = *explicit_match;
            } else {
                LOG_WARN("[registry] requested model preset '" + opts.model_name +
                         "' not found; falling back to default saved model");
                profile = resolve_effective_model(*deps.config, std::nullopt, std::nullopt);
            }
        } else if (resumed_meta) {
            if (!resumed_meta->model_preset.empty() &&
                find_profile_by_name(*deps.config, resumed_meta->model_preset) == nullptr) {
                LOG_WARN("[registry] session model preset '" + resumed_meta->model_preset +
                         "' was deleted from saved_models");
                ResolvedSessionModel deleted;
                deleted.state = deleted_state_from_name(resumed_meta->model_preset);
                return deleted;
            }
            profile = resolve_effective_model(
                *deps.config, cwd_override, std::optional<SessionMeta>{*resumed_meta});
        } else {
            profile = resolve_effective_model(*deps.config, cwd_override, std::nullopt);
        }
        return resolve_from_profile(*deps.config, profile);
    }

    auto [provider, model] = current_provider_model(deps, opts.model_name);
    ResolvedSessionModel resolved;
    resolved.provider = deps.provider_accessor ? deps.provider_accessor() : nullptr;
    resolved.state.name = opts.model_name;
    resolved.state.provider = provider;
    resolved.state.model = model;
    resolved.state.context_window = 0;
    return resolved;
}

SessionOptions with_resolved_workspace(const SessionRegistryDeps& deps,
                                       const SessionOptions& in,
                                       const std::string& session_id = {}) {
    SessionOptions out = in;
    if (out.no_workspace) {
        if (out.cwd.empty()) {
            out.cwd = no_workspace_session_cwd(session_id, deps.no_workspace_cache_root);
        }
        out.workspace_hash.clear();
        return out;
    }
    if (out.cwd.empty()) {
        out.cwd = deps.cwd;
    }
    if (out.workspace_hash.empty() && !out.cwd.empty()) {
        out.workspace_hash = compute_cwd_hash(out.cwd);
    }
    return out;
}

std::string trim_ascii(std::string s) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && is_space(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && is_space(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

std::string lower_ascii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

bool parse_goal_budget_value(const std::string& text, std::int64_t* out) {
    if (!out || text.empty()) return false;
    std::string s = lower_ascii(text);
    std::int64_t multiplier = 1;
    if (!s.empty() && (s.back() == 'k' || s.back() == 'm')) {
        multiplier = s.back() == 'k' ? 1000 : 1000000;
        s.pop_back();
    }
    if (s.empty()) return false;
    std::int64_t value = 0;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        value = value * 10 + (c - '0');
    }
    *out = value * multiplier;
    return *out > 0;
}

PermissionMode permission_mode_from_name(std::string mode) {
    if (mode == "acceptEdits") mode = "accept-edits";
    if (mode == "accept-edits") return PermissionMode::AcceptEdits;
    if (mode == "yolo") return PermissionMode::Yolo;
    if (mode == "plan") return PermissionMode::Plan;
    return PermissionMode::Default;
}

void emit_session_title_updated(SessionEntry& entry) {
    if (!entry.loop || !entry.sm) return;
    entry.loop->events().emit(SessionEventKind::SessionUpdated, nlohmann::json{
        {"session_id", entry.id},
        {"workspace_hash", entry.workspace_hash},
        {"cwd", entry.cwd},
        {"title", entry.sm->current_title()},
        {"title_source", entry.sm->current_title_source()},
    });
}

struct RegistryGoalArgs {
    std::optional<std::int64_t> token_budget;
    std::string remainder;
    std::string error;
};

RegistryGoalArgs parse_registry_goal_args(std::string args) {
    RegistryGoalArgs parsed;
    args = trim_ascii(std::move(args));
    if (args.rfind("--tokens", 0) != 0) {
        parsed.remainder = args;
        return parsed;
    }
    std::string rest = trim_ascii(args.substr(std::string("--tokens").size()));
    const auto split = rest.find_first_of(" \t\r\n");
    const std::string budget_text = split == std::string::npos ? rest : rest.substr(0, split);
    std::int64_t budget = 0;
    if (!parse_goal_budget_value(budget_text, &budget)) {
        parsed.error = "Goal token budget must be a positive integer, optionally suffixed with K or M.";
        return parsed;
    }
    parsed.token_budget = budget;
    parsed.remainder = split == std::string::npos ? std::string{} : trim_ascii(rest.substr(split + 1));
    return parsed;
}

std::string format_registry_goal_summary(const ThreadGoal& goal) {
    std::ostringstream oss;
    oss << "Goal:\n"
        << "  objective: " << goal.objective << "\n"
        << "  status:    " << to_string(goal.status) << "\n"
        << "  tokens:    " << goal.tokens_used;
    if (goal.token_budget.has_value()) {
        oss << " / " << *goal.token_budget
            << " (" << std::max<std::int64_t>(0, *goal.token_budget - goal.tokens_used)
            << " remaining)";
    }
    oss << "\n  elapsed:   " << goal.time_used_seconds << "s";
    return oss.str();
}

void emit_goal_audit_message(SessionEntry& entry,
                             const ThreadGoal& goal,
                             const std::string& action,
                             const std::string& label) {
    if (!entry.loop) return;
    entry.loop->emit_transcript_system_message(
        "[Goal] " + label + ": " + goal.objective,
        nlohmann::json{
            {"goal_audit", true},
            {"goal_action", action},
            {"goal_id", goal.goal_id},
            {"thread_id", goal.thread_id},
        });
}

std::optional<ThreadGoal> current_active_goal(SessionEntry& entry) {
    if (!entry.sm) return std::nullopt;
    const std::string sid = entry.sm->current_session_id();
    ThreadGoalStore* store = entry.sm->existing_goal_store();
    if (!store || sid.empty()) return std::nullopt;
    std::string error;
    auto goal = store->get_thread_goal(sid, &error);
    if (!error.empty() || !goal.has_value() || goal->status != ThreadGoalStatus::Active) {
        return std::nullopt;
    }
    return goal;
}

BuiltinCommandResult execute_goal_builtin(SessionEntry& entry,
                                          const BuiltinCommandRequest& request) {
    if (!entry.sm || !entry.loop) return {BuiltinCommandStatus::Failed, "session unavailable"};
    ThreadGoalStore* store = entry.sm->goal_store();
    if (!store) {
        entry.loop->emit_system_message("Goal storage is not available.");
        return {BuiltinCommandStatus::Failed, "goal storage unavailable"};
    }

    const std::string args = trim_ascii(request.args);
    const std::string lower = lower_ascii(args);
    std::string sid = entry.sm->current_session_id();
    std::string error;

    auto emit_updated = [&entry](const ThreadGoal& goal) {
        entry.loop->events().emit(SessionEventKind::GoalUpdated,
            nlohmann::json{{"session_id", goal.thread_id}, {"goal", thread_goal_to_json(goal)}});
        entry.loop->restore_goal_runtime();
    };
    auto emit_cleared = [&entry](const std::string& session_id) {
        entry.loop->events().emit(SessionEventKind::GoalCleared,
            nlohmann::json{{"session_id", session_id}});
        entry.loop->restore_goal_runtime();
    };

    if (args.empty() || lower == "view") {
        if (sid.empty()) {
            entry.loop->emit_system_message("No goal set. Use /goal <objective> to create one.");
            return {BuiltinCommandStatus::Accepted, "completed"};
        }
        auto goal = store->get_thread_goal(sid, &error);
        if (!error.empty()) {
            entry.loop->emit_system_message("Goal error: " + error);
            return {BuiltinCommandStatus::Failed, error};
        }
        entry.loop->emit_system_message(goal.has_value()
            ? format_registry_goal_summary(*goal)
            : "No goal set. Use /goal <objective> to create one.");
        return {BuiltinCommandStatus::Accepted, "completed"};
    }

    const auto first_space = args.find_first_of(" \t\r\n");
    const std::string sub = lower_ascii(first_space == std::string::npos
        ? args
        : args.substr(0, first_space));
    const std::string tail = first_space == std::string::npos
        ? std::string{}
        : trim_ascii(args.substr(first_space + 1));

    const bool state_only = sub == "clear" || sub == "pause" || sub == "resume" || sub == "edit";
    if (!state_only) sid = entry.sm->ensure_active_session_id();
    if (sid.empty()) {
        entry.loop->emit_system_message("No active session is available for /goal.");
        return {BuiltinCommandStatus::Failed, "no active session"};
    }
    auto current = store->get_thread_goal(sid, &error);
    if (!error.empty()) {
        entry.loop->emit_system_message("Goal error: " + error);
        return {BuiltinCommandStatus::Failed, error};
    }

    if (sub == "clear") {
        if (!current.has_value()) {
            entry.loop->emit_system_message("No goal to clear.");
            return {BuiltinCommandStatus::Accepted, "completed"};
        }
        if (!store->delete_thread_goal(sid, &error)) {
            entry.loop->emit_system_message("Goal error: " + error);
            return {BuiltinCommandStatus::Failed, error};
        }
        emit_cleared(sid);
        entry.loop->emit_system_message("Goal cleared.");
        return {BuiltinCommandStatus::Accepted, "completed"};
    }

    if (sub == "pause") {
        if (!current.has_value() || current->status != ThreadGoalStatus::Active) {
            entry.loop->emit_system_message("Goal is not active.");
            return {BuiltinCommandStatus::Accepted, "completed"};
        }
        if (!store->update_thread_goal_status(sid, current->goal_id, ThreadGoalStatus::Paused, &error)) {
            entry.loop->emit_system_message("Goal error: " + error);
            return {BuiltinCommandStatus::Failed, error};
        }
        auto goal = store->get_thread_goal(sid);
        if (goal.has_value()) emit_updated(*goal);
        entry.loop->emit_system_message("Goal paused.");
        return {BuiltinCommandStatus::Accepted, "completed"};
    }

    if (sub == "resume") {
        if (!current.has_value()) {
            entry.loop->emit_system_message("No goal to resume.");
            return {BuiltinCommandStatus::Accepted, "completed"};
        }
        if (current->status == ThreadGoalStatus::Complete) {
            entry.loop->emit_system_message("Goal is already complete.");
            return {BuiltinCommandStatus::Accepted, "completed"};
        }
        if (current->token_budget.has_value() && current->tokens_used >= *current->token_budget) {
            entry.loop->emit_system_message("Goal is over its token budget. Create a replacement goal with a larger budget.");
            return {BuiltinCommandStatus::Accepted, "completed"};
        }
        if (!store->update_thread_goal_status(sid, current->goal_id, ThreadGoalStatus::Active, &error)) {
            entry.loop->emit_system_message("Goal error: " + error);
            return {BuiltinCommandStatus::Failed, error};
        }
        auto goal = store->get_thread_goal(sid);
        if (goal.has_value()) emit_updated(*goal);
        entry.loop->emit_system_message("Goal resumed.");
        if (goal.has_value()) {
            emit_goal_audit_message(entry, *goal, "resume", "Resumed");
        }
        entry.loop->clear_stale_abort_request();
        entry.loop->maybe_continue_goal();
        return {BuiltinCommandStatus::Accepted, "completed"};
    }

    if (sub == "edit") {
        if (!current.has_value()) {
            entry.loop->emit_system_message("No goal to edit.");
            return {BuiltinCommandStatus::Accepted, "completed"};
        }
        auto parsed = parse_registry_goal_args(tail);
        if (!parsed.error.empty()) {
            entry.loop->emit_system_message(parsed.error);
            return {BuiltinCommandStatus::Failed, parsed.error};
        }
        const std::string objective = trim_goal_objective(parsed.remainder);
        if (!validate_goal_objective(objective, &error)) {
            entry.loop->emit_system_message(error);
            return {BuiltinCommandStatus::Failed, error};
        }
        auto budget = parsed.token_budget.has_value() ? parsed.token_budget : current->token_budget;
        if (!store->update_thread_goal_objective(sid, current->goal_id, objective, budget, &error)) {
            entry.loop->emit_system_message("Goal error: " + error);
            return {BuiltinCommandStatus::Failed, error};
        }
        auto goal = store->get_thread_goal(sid);
        if (goal.has_value()) emit_updated(*goal);
        entry.loop->emit_system_message(goal.has_value() ? format_registry_goal_summary(*goal) : "Goal updated.");
        // 回合运行中时把新 objective 通知给正在跑的模型(objective_updated
        // steering);空闲时 no-op,下一次 continuation 自然携带新 objective。
        entry.loop->notify_goal_objective_updated();
        return {BuiltinCommandStatus::Accepted, "completed"};
    }

    auto parsed = parse_registry_goal_args(args);
    if (!parsed.error.empty()) {
        entry.loop->emit_system_message(parsed.error);
        return {BuiltinCommandStatus::Failed, parsed.error};
    }
    const std::string objective = trim_goal_objective(parsed.remainder);
    if (!validate_goal_objective(objective, &error)) {
        entry.loop->emit_system_message(error);
        return {BuiltinCommandStatus::Failed, error};
    }
    if (!store->replace_thread_goal(sid, objective, parsed.token_budget, ThreadGoalStatus::Active, &error)) {
        entry.loop->emit_system_message("Goal error: " + error);
        return {BuiltinCommandStatus::Failed, error};
    }
    auto goal = store->get_thread_goal(sid);
    if (goal.has_value()) emit_updated(*goal);
    entry.loop->emit_system_message(goal.has_value() ? format_registry_goal_summary(*goal) : "Goal created.");
    if (goal.has_value()) {
        emit_goal_audit_message(entry, *goal, "create", "Started");
    }
    entry.loop->maybe_continue_goal();
    return {BuiltinCommandStatus::Accepted, "completed"};
}

BuiltinCommandResult execute_plan_builtin(SessionEntry& entry,
                                          const BuiltinCommandRequest& request) {
    if (!entry.sm || !entry.loop || !entry.perm) {
        return {BuiltinCommandStatus::Failed, "session unavailable"};
    }

    const PermissionMode before = entry.perm->mode();
    entry.perm->set_mode(PermissionMode::Plan);
    entry.perm->clear_session_allows();
    entry.sm->set_permission_mode("plan");
    entry.sm->set_pre_plan_permission_mode(PermissionManager::mode_name(
        before == PermissionMode::Plan ? entry.perm->pre_plan_mode() : before));
    const std::string plan_file = entry.sm->ensure_plan_file_path();

    std::ostringstream oss;
    oss << "Plan mode enabled.";
    if (!plan_file.empty()) {
        oss << "\nPlan file: " << plan_file;
    }
    oss << "\nExplore and update only the plan file, then call ExitPlanMode for approval.";
    entry.loop->emit_system_message(oss.str());

    const std::string args = trim_ascii(request.args);
    if (!args.empty()) {
        const std::string display = request.display_text.empty()
            ? "/plan " + args
            : request.display_text;
        entry.loop->submit(args, display);
        return {BuiltinCommandStatus::Accepted, "queued"};
    }

    return {BuiltinCommandStatus::Accepted, "completed"};
}

} // namespace

std::string default_no_workspace_cache_root() {
    return path_to_utf8(path_from_utf8(get_acecode_dir()) / "cache" / "no-workspace");
}

std::string no_workspace_session_cwd(const std::string& session_id,
                                     const std::string& cache_root) {
    const std::string root = cache_root.empty() ? default_no_workspace_cache_root() : cache_root;
    return path_to_utf8(path_from_utf8(root) / session_dir_name_from_id(session_id));
}

std::vector<std::string> list_no_workspace_session_cwds(const std::string& cache_root) {
    namespace fs = std::filesystem;
    std::vector<std::string> out;
    const fs::path root = path_from_utf8(cache_root.empty()
        ? default_no_workspace_cache_root()
        : cache_root);
    std::error_code ec;
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) return out;

    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code item_ec;
        if (!it->is_directory(item_ec) || item_ec) continue;
        out.push_back(path_to_utf8(it->path()));
    }
    std::sort(out.begin(), out.end());
    return out;
}

SessionRegistry::SessionRegistry(SessionRegistryDeps deps)
    : deps_(std::move(deps)) {}

SessionRegistry::~SessionRegistry() {
    if (deps_.power_guard) {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& [id, _entry] : entries_) {
            deps_.power_guard->release_session(id);
        }
    }
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lk(title_threads_mu_);
        threads.swap(title_threads_);
    }
    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }
    // entries_ 析构会触发每个 SessionEntry 析构 → AgentLoop::shutdown 等等
    // worker thread join。锁不需要 — 此时没人再调 lookup/destroy(daemon
    // 退出路径)。
}

std::string SessionRegistry::create(const SessionOptions& opts) {
    std::string id = opts.preset_session_id.empty()
        ? SessionStorage::generate_session_id()
        : opts.preset_session_id;
    SessionOptions create_opts = opts;
    if (create_opts.no_workspace) create_opts.cwd.clear();
    SessionOptions resolved = with_resolved_workspace(deps_, create_opts, id);

    auto entry = make_entry_locked(id, resolved, nullptr);
    {
        std::lock_guard<std::mutex> lk(mu_);
        entries_.emplace(id, std::move(entry));
    }
    if (auto active = acquire(id)) {
        if (active->loop) active->loop->dispatch_session_start_hook("startup");
    }
    if (resolved.auto_start && !resolved.initial_user_message.empty()) {
        UserInput input;
        input.text = resolved.initial_user_message;
        maybe_start_auto_title(id, input);
        if (auto active = acquire(id)) {
            if (active->loop) active->loop->submit(input);
        }
    }
    LOG_INFO("[registry] created session " + id);
    return id;
}

std::shared_ptr<SessionEntry>
SessionRegistry::make_entry_locked(const std::string& id,
                                   const SessionOptions& opts,
                                   const SessionMeta* resumed_meta) {
    auto resolved_model = resolve_session_model(deps_, opts, resumed_meta);

    auto entry = std::make_shared<SessionEntry>();
    entry->id = id;
    entry->cwd = opts.cwd.empty() ? deps_.cwd : opts.cwd;
    entry->subagent_depth = opts.subagent_depth;
    entry->parent_session_id = opts.parent_session_id;
    if (resumed_meta && !resumed_meta->parent_session_id.empty()) {
        // resume 路径:子会话身份从持久化 meta 恢复,深度限制随之生效。
        entry->parent_session_id = resumed_meta->parent_session_id;
    }
    if (!entry->parent_session_id.empty() && entry->subagent_depth < 1) {
        entry->subagent_depth = 1;
    }
    entry->no_workspace = opts.no_workspace || (resumed_meta && resumed_meta->no_workspace);
    if (entry->no_workspace && !entry->cwd.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(path_from_utf8(entry->cwd), ec);
        if (ec) {
            LOG_WARN("[registry] failed to create no-workspace cwd " +
                     entry->cwd + ": " + ec.message());
        }
    }
    entry->workspace_hash = entry->no_workspace
        ? std::string{}
        : (opts.workspace_hash.empty()
        ? compute_cwd_hash(entry->cwd)
        : opts.workspace_hash);
    entry->provider = resolved_model.state.provider;
    entry->model = resolved_model.state.model;
    entry->model_state = resolved_model.state;
    entry->provider_slot = std::make_shared<SessionEntry::ProviderSlot>();
    entry->provider_slot->provider = std::move(resolved_model.provider);
    if (deps_.config) {
        entry->skill_registry = std::make_unique<SkillRegistry>();
        initialize_skill_registry(*entry->skill_registry, *deps_.config, entry->cwd);
    }

    // SessionManager
    entry->sm = std::make_unique<SessionManager>();
    entry->sm->start_session(entry->cwd,
                             entry->model_state.provider,
                             entry->model_state.model,
                             id,
                             entry->model_state.name,
                             "daemon",
                             entry->no_workspace);
    if (!entry->parent_session_id.empty()) {
        // 子会话身份写进 meta(lazy:首条消息落盘时随初始 meta 一起写)。
        entry->sm->set_parent_session_id(entry->parent_session_id);
    }

    // PermissionManager: 复制 mode + dangerous flag,rules 由调用方在初始化
    // template_permissions 时设好。session_allowed_ 不复制,各 session 独立。
    entry->perm = std::make_unique<PermissionManager>();
    if (deps_.template_permissions) {
        entry->perm->set_mode(deps_.template_permissions->mode());
        entry->perm->set_dangerous(deps_.template_permissions->is_dangerous());
        // 注意: rules 当前没有 copy 接口 — v1 暂不复制 rules,daemon 路径
        // 自己装(后续 Section 9 落 HTTP 时一起补)。TUI 路径不受影响。
    }
    // 显式传入的 permission_mode 优先于 resume meta 恢复值:headless
    // `-p --resume <id> --permission-mode accept-edits` 若被静默忽略,脚本
    // 会在 default 模式下被写权限门自动拒绝,极难排查。web resume 不传该
    // 字段(entry_opts 为空串),仍走 meta 恢复分支。
    if (!opts.permission_mode.empty()) {
        const PermissionMode requested_mode =
            permission_mode_from_name(opts.permission_mode);
        if (requested_mode == PermissionMode::Plan) {
            entry->perm->set_mode(PermissionMode::Default);
            entry->perm->set_mode(PermissionMode::Plan);
        } else {
            entry->perm->set_mode(requested_mode);
        }
    } else if (resumed_meta) {
        const PermissionMode restored_mode =
            permission_mode_from_name(resumed_meta->permission_mode);
        if (restored_mode == PermissionMode::Plan) {
            entry->perm->set_mode(permission_mode_from_name(
                resumed_meta->pre_plan_permission_mode.empty()
                    ? std::string{"default"}
                    : resumed_meta->pre_plan_permission_mode));
            entry->perm->set_mode(PermissionMode::Plan);
        } else {
            entry->perm->set_mode(restored_mode);
        }
    }
    entry->sm->set_permission_mode(
        PermissionManager::mode_name(entry->perm->mode()),
        /*persist_immediately=*/false);
    if (entry->perm->mode() == PermissionMode::Plan) {
        entry->sm->set_pre_plan_permission_mode(
            PermissionManager::mode_name(entry->perm->pre_plan_mode()),
            /*persist_immediately=*/false);
        entry->sm->ensure_plan_file_path();
    }

    // AgentLoop: daemon 全走 events_,but busy transitions still feed the
    // process power guard so web/desktop/background sessions keep the host awake.
    AgentCallbacks empty_cb;
    if (deps_.power_guard) {
        auto* power_guard = deps_.power_guard;
        empty_cb.on_busy_changed = [power_guard, id](bool busy) {
            power_guard->set_busy(id, busy);
        };
    }
    auto slot = entry->provider_slot;
    AgentLoop::ProviderAccessor provider_accessor = [slot]() -> std::shared_ptr<LlmProvider> {
        if (!slot) return {};
        std::lock_guard<std::mutex> lk(slot->mu);
        return slot->provider;
    };
    entry->loop = std::make_unique<AgentLoop>(
        provider_accessor,
        *deps_.tools,
        std::move(empty_cb),
        entry->cwd,
        *entry->perm);
    entry->loop->set_no_model_config_prompt(
        u8"请先配置大模型服务。请打开 设置 > 模型 添加模型。");

    if (deps_.config) {
        entry->loop->set_context_window(entry->model_state.context_window > 0
            ? entry->model_state.context_window
            : deps_.config->context_window);
        entry->loop->set_agent_loop_config(deps_.config->agent_loop);
    }
    entry->loop->set_session_manager(entry->sm.get());
    entry->loop->set_hook_manager(deps_.hook_manager);
    entry->loop->set_skill_registry(entry->skill_registry
        ? entry->skill_registry.get()
        : deps_.skill_registry);
    entry->loop->set_memory_registry(deps_.memory_registry);
    entry->loop->set_memory_config(deps_.memory_cfg);
    entry->loop->set_project_instructions_config(deps_.project_instructions_cfg);
    entry->loop->set_custom_instructions_config(deps_.custom_instructions_cfg);
    if (deps_.config) {
        entry->loop->set_git_context_config(&deps_.config->git_context);
    }

    // PermissionPrompter: 异步 — 触发 PermissionRequest 事件,等浏览器 decision
    auto prompter = std::make_unique<AsyncPrompter>(entry->loop->events());
    entry->prompter = prompter.get();
    entry->loop->set_permission_prompter(std::move(prompter));

    // AskUserQuestionPrompter: 异步 — 触发 QuestionRequest 事件,等浏览器
    // question_answer 回流。AgentLoop 在每次工具执行时把 prompter 包成
    // ToolContext::ask_user_questions 回调注入(set_ask_question_prompter)。
    entry->ask_prompter = std::make_unique<AskUserQuestionPrompter>(entry->loop->events());
    entry->loop->set_ask_question_prompter(entry->ask_prompter.get());

    return entry;
}

void SessionRegistry::restore_loop_history(
    SessionEntry& entry,
    const std::vector<ChatMessage>& messages) const {
    if (!entry.loop) return;
    restore_file_tool_state_from_messages(messages);
    entry.loop->clear_messages();

    if (auto checkpoint = latest_valid_compact_checkpoint(messages)) {
        append_model_messages_to_loop(*entry.loop, checkpoint->second.replacement_history);
        if (checkpoint->first + 1 < messages.size()) {
            std::vector<ChatMessage> suffix(
                messages.begin() + static_cast<std::ptrdiff_t>(checkpoint->first + 1),
                messages.end());
            append_model_messages_to_loop(*entry.loop, suffix);
        }
    } else {
        append_model_messages_to_loop(*entry.loop, messages);
    }
}

bool SessionRegistry::resume(const std::string& id, const SessionOptions& opts) {
    if (id.empty()) return false;
    SessionOptions resolved = with_resolved_workspace(deps_, opts, id);

    {
        std::lock_guard<std::mutex> lk(mu_);
        if (entries_.find(id) != entries_.end()) {
            return true;
        }
    }

    SessionManager meta_reader;
    auto [provider, model] = current_provider_model(deps_, "");
    meta_reader.start_session(resolved.cwd, provider, model, "", "", "daemon");
    SessionMeta meta = meta_reader.load_session_meta(id);

    if (meta.id.empty()) {
        if (meta_reader.has_incompatible_session_data(id)) {
            LOG_WARN("[registry] resume " + id +
                     " rejected incompatible PID-suffixed old data; delete old project session data under ~/.acecode/projects");
        }
        return false;
    }

    SessionOptions entry_opts;
    entry_opts.cwd = resolved.cwd;
    entry_opts.no_workspace = meta.no_workspace || resolved.no_workspace;
    entry_opts.workspace_hash = entry_opts.no_workspace ? std::string{} : resolved.workspace_hash;
    // headless -p --resume 允许 --model / --permission-mode 覆盖会话保存值
    //(resolve_session_model 与 make_entry_locked 都是"显式指定优先于 meta");
    // web resume 不传这两个字段,行为不变。
    entry_opts.model_name = resolved.model_name;
    entry_opts.permission_mode = resolved.permission_mode;
    auto entry = make_entry_locked(id, entry_opts, &meta);
    auto messages = entry->sm->resume_session(id);
    if (!entry->sm->last_error().empty()) {
        LOG_WARN("[registry] resume " + id + " failed: " + entry->sm->last_error());
        return false;
    }
    restore_loop_history(*entry, messages);
    // worktree 会话恢复:meta 记录的 worktree 目录还在就把 AgentLoop 的
    // 工作目录切回去(SessionEntry::cwd / 会话存储位置不动 —— worktree
    // 是同一个项目的临时工作区);目录已被外部删除则清状态,避免
    // ExitWorktree 操作幽灵路径。
    {
        const WorktreeSessionInfo resumed_worktree = entry->sm->active_worktree();
        if (resumed_worktree.active()) {
            std::error_code wt_ec;
            if (std::filesystem::exists(
                    path_from_utf8(resumed_worktree.worktree_path), wt_ec)) {
                entry->loop->set_cwd(resumed_worktree.worktree_path);
            } else {
                entry->sm->clear_active_worktree();
                LOG_WARN("[registry] resume " + id + " worktree missing: " +
                         resumed_worktree.worktree_path + "; cleared worktree state");
            }
        }
    }
    entry->loop->publish_current_goal_state();
    if (auto goal = current_active_goal(*entry)) {
        emit_goal_audit_message(*entry, *goal, "session_resume", "Continuing");
    }
    entry->loop->maybe_continue_goal();
    entry->loop->dispatch_session_start_hook("resume");

    {
        std::lock_guard<std::mutex> lk(mu_);
        entries_.emplace(id, std::move(entry));
    }
    LOG_INFO("[registry] resumed session " + id);
    return true;
}

std::shared_ptr<SessionEntry> SessionRegistry::acquire(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = entries_.find(id);
    return it == entries_.end() ? nullptr : it->second;
}

SessionEntry* SessionRegistry::lookup(const std::string& id) {
    auto entry = acquire(id);
    return entry ? entry.get() : nullptr;
}

BuiltinCommandResult SessionRegistry::execute_builtin_command(
    const std::string& id,
    const BuiltinCommandRequest& request) {
    if (request.name != "init" && request.name != "compact" &&
        request.name != "goal" && request.name != "plan" &&
        request.name != "lsp") {
        return {BuiltinCommandStatus::UnsupportedCommand, "unsupported command"};
    }

    auto entry = acquire(id);
    if (!entry || !entry->loop) {
        return {BuiltinCommandStatus::UnknownSession, "unknown session"};
    }

    if (request.name == "compact") {
        entry->loop->submit_compact();
        return {BuiltinCommandStatus::Accepted, "queued"};
    }

    if (request.name == "goal") {
        return execute_goal_builtin(*entry, request);
    }

    if (request.name == "plan") {
        return execute_plan_builtin(*entry, request);
    }

    if (request.name == "lsp") {
        // 与 TUI /lsp 共用同一份文本(dispatch_lsp_subcommand),经会话
        // system message 透出到 Web 聊天流。
        if (!entry->loop) return {BuiltinCommandStatus::Failed, "session unavailable"};
        entry->loop->emit_system_message(
            dispatch_lsp_subcommand(trim_ascii(request.args)));
        return {BuiltinCommandStatus::Accepted, "ok"};
    }

    const std::filesystem::path cwd = path_from_utf8(entry->cwd);
    const std::filesystem::path target = cwd / "AGENT.md";

    bool provider_usable = false;
    if (entry->provider_slot) {
        std::lock_guard<std::mutex> lk(entry->provider_slot->mu);
        provider_usable = static_cast<bool>(entry->provider_slot->provider);
    }
    if (!provider_usable) {
        std::error_code ec;
        if (std::filesystem::exists(target, ec)) {
            entry->loop->emit_system_message(
                "AGENT.md already exists at " + path_to_utf8_generic(target) +
                " - no model is configured, so /init cannot propose improvements. "
                "Edit it by hand, or run /configure first and re-run /init to get "
                "an LLM-driven improvement pass.");
            return {BuiltinCommandStatus::Accepted, "completed"};
        }

        std::ofstream ofs(target, std::ios::binary);
        if (!ofs.is_open()) {
            entry->loop->emit_system_message(
                "Failed to open " + path_to_utf8_generic(target) + " for writing.");
            return {BuiltinCommandStatus::Failed, "failed to open AGENT.md for writing"};
        }
        ofs << build_agent_md_skeleton(cwd);
        entry->loop->emit_system_message(
            "Created " + path_to_utf8_generic(target) +
            " (offline skeleton - no model is configured, run /configure to get "
            "a filled-in version).");
        return {BuiltinCommandStatus::Accepted, "completed"};
    }

    entry->loop->emit_system_message(
        "[Invoking /init - analyzing codebase and authoring AGENT.md...]");
    const std::string display = request.display_text.empty()
        ? std::string{"/init"}
        : request.display_text;
    entry->loop->submit(build_init_prompt(cwd), display);
    return {BuiltinCommandStatus::Accepted, "queued"};
}

std::optional<SessionModelState>
SessionRegistry::current_model_state(const std::string& id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = entries_.find(id);
    if (it == entries_.end() || !it->second) return std::nullopt;
    auto state = it->second->model_state;
    if (deps_.config) {
        mark_deleted_if_model_name_missing(*deps_.config, state);
    }
    return state;
}

bool SessionRegistry::model_profile_used_by_busy_session(const std::string& model_name) const {
    if (model_name.empty()) return false;
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& [id, entry] : entries_) {
        (void)id;
        if (!entry || entry->model_state.name != model_name || !entry->loop) continue;
        if (entry->loop->is_busy()) return true;
    }
    return false;
}

std::optional<PermissionMode>
SessionRegistry::permission_mode(const std::string& id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = entries_.find(id);
    if (it == entries_.end() || !it->second || !it->second->perm) return std::nullopt;
    return it->second->perm->mode();
}

bool SessionRegistry::set_permission_mode(const std::string& id, PermissionMode mode) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = entries_.find(id);
    if (it == entries_.end() || !it->second || !it->second->perm) return false;
    it->second->perm->set_mode(mode);
    it->second->perm->clear_session_allows();
    if (it->second->sm) {
        it->second->sm->set_permission_mode(PermissionManager::mode_name(mode));
        if (mode == PermissionMode::Plan) {
            it->second->sm->set_pre_plan_permission_mode(
                PermissionManager::mode_name(it->second->perm->pre_plan_mode()));
            it->second->sm->ensure_plan_file_path();
        } else {
            it->second->sm->set_pre_plan_permission_mode(std::string{});
        }
    }
    if (mode == PermissionMode::Yolo && it->second->prompter) {
        it->second->prompter->resolve_all(PermissionDecisionChoice::Allow);
    }
    return true;
}

void SessionRegistry::maybe_start_auto_title(const std::string& id, const UserInput& input) {
    if (!deps_.config || !deps_.config->session_title.enabled) return;
    std::string text = visible_auto_title_input(input);
    if (text.empty()) return;

    std::optional<ModelProfile> profile;
    std::string session_id;
    const AppConfig* cfg = deps_.config;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = entries_.find(id);
        if (it == entries_.end() || !it->second || !it->second->sm) return;
        auto& entry = *it->second;
        profile = resolve_auto_title_profile(*deps_.config,
                                             entry.model_state.name,
                                             entry.cwd);
        if (!profile.has_value()) return;
        if (!entry.sm->mark_auto_title_generation_started()) return;
        session_id = entry.id;
    }

    std::thread worker([this, cfg, profile = std::move(*profile),
                        session_id, text = std::move(text)]() mutable {
        try {
            auto provider = create_auto_title_provider(std::move(profile), *cfg);
            if (!provider) return;
            auto title = generate_auto_session_title(*provider, text, *cfg);
            if (!title.has_value() || title->empty()) return;
            auto entry = acquire(session_id);
            if (!entry || !entry->sm) return;
            if (entry->sm->try_set_generated_session_title(*title)) {
                emit_session_title_updated(*entry);
            }
        } catch (const std::exception& e) {
            LOG_WARN("[registry] auto session title generation failed: " +
                     std::string(e.what()));
        } catch (...) {
            LOG_WARN("[registry] auto session title generation failed");
        }
    });
    std::lock_guard<std::mutex> lk(title_threads_mu_);
    title_threads_.push_back(std::move(worker));
}

PermissionMode SessionRegistry::default_permission_mode() const {
    if (!deps_.template_permissions) return PermissionMode::Default;
    return deps_.template_permissions->mode();
}

void SessionRegistry::set_default_permission_mode(PermissionMode mode) {
    if (!deps_.template_permissions) return;
    if (mode == PermissionMode::Plan) {
        deps_.template_permissions->set_mode(PermissionMode::Default);
    }
    deps_.template_permissions->set_mode(mode);
    deps_.template_permissions->clear_session_allows();
}

bool SessionRegistry::switch_model(const std::string& id,
                                   const ModelProfile& profile,
                                   SessionModelState* out,
                                   std::string* error) {
    if (!deps_.config) {
        if (error) *error = "config unavailable";
        return false;
    }

    // Phase 1: under mu_, find the entry and capture per-session deps.
    // We deliberately release mu_ before calling apply_model_to_session
    // because the helper may do blocking I/O (Copilot silent_auth HTTPS
    // exchange, set_active_provider meta write). Holding mu_ across that
    // would block list_active / lookup / other switch_model calls in
    // every active session.
    std::shared_ptr<SessionEntry> entry;
    std::shared_ptr<SessionEntry::ProviderSlot> slot;
    SessionManager* sm = nullptr;
    AgentLoop* loop = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = entries_.find(id);
        if (it == entries_.end() || !it->second) {
            if (error) *error = "session not found";
            return false;
        }
        entry = it->second;
        if (!entry->provider_slot) {
            entry->provider_slot = std::make_shared<SessionEntry::ProviderSlot>();
        }
        slot = entry->provider_slot;
        sm = entry->sm.get();
        loop = entry->loop.get();
    }

    // Phase 2: lock-free helper call. provider_slot has its own internal
    // mutex; sm and loop are stable for the lifetime of the SessionEntry,
    // and apply_model_to_session does not access the registry.
    ApplyModelDeps deps;
    deps.provider_slot = slot.get();
    deps.sm = sm;
    deps.loop = loop;
    deps.cfg = const_cast<AppConfig*>(deps_.config);

    ApplyModelResult result;
    try {
        result = apply_model_to_session(profile, deps);
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }

    // Phase 3: re-take mu_ to publish state into the entry. If the entry
    // got destroyed in between, the helper already mutated the slot but
    // the state writeback is moot — return success with the resolved
    // state for the caller to consume.
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = entries_.find(id);
        if (it != entries_.end() && it->second) {
            auto& entry = *it->second;
            entry.model_state = result.state;
            entry.provider = result.state.provider;
            entry.model = result.state.model;
        }
    }

    if (out) *out = result.state;
    // 注意:result.warning(silent_auth / meta-persist 非致命退化)在
    // 成功路径上不再写到 *error — error 只在失败时填,保持与重构前的
    // 接口契约一致。warning 字段当前只走 LOG_WARN;若以后 HTTP handler
    // 想把 warning 透给 UI,改 switch_model 签名加 string* warning。
    return true;
}

std::optional<SessionModelState>
SessionRegistry::model_state_from_meta(const SessionMeta& meta) const {
    if (meta.id.empty() || !deps_.config) return std::nullopt;
    if (!meta.model_preset.empty() &&
        find_profile_by_name(*deps_.config, meta.model_preset) == nullptr) {
        return deleted_state_from_name(meta.model_preset);
    }
    auto profile = resolve_effective_model(
        *deps_.config, std::nullopt, std::optional<SessionMeta>{meta});
    auto state = state_from_profile(*deps_.config, profile);
    mark_deleted_if_model_name_missing(*deps_.config, state);
    return state;
}

void SessionRegistry::destroy(const std::string& id) {
    std::shared_ptr<SessionEntry> moved;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = entries_.find(id);
        if (it == entries_.end()) return;
        moved = std::move(it->second);
        entries_.erase(it);
    }
    if (deps_.power_guard) deps_.power_guard->release_session(id);
    // 析构走出锁外: AgentLoop::shutdown 会 join worker,可能耗时(等当前
    // tool 跑完)。在锁内会阻塞所有别的 create/lookup/destroy。
    if (moved && moved->loop) moved->loop->abort();
    moved.reset();
    LOG_INFO("[registry] destroyed session " + id);
}

std::vector<SessionInfo> SessionRegistry::list_active() const {
    std::vector<SessionInfo> out;
    std::lock_guard<std::mutex> lk(mu_);
    out.reserve(entries_.size());
    for (const auto& [id, entry] : entries_) {
        SessionInfo info;
        info.id = id;
        info.cwd = entry->cwd;
        info.workspace_hash = entry->workspace_hash;
        info.active = true;
        info.no_workspace = entry->no_workspace;
        info.parent_session_id = entry->parent_session_id;
        if (entry->loop) info.busy = entry->loop->is_busy();
        if (entry->sm) {
            // SessionManager 没有公开的 created_at / updated_at 接口,从 meta
            // 拿:这里**可选**调 load_session_meta 走磁盘读,有 IO 成本。
            // v1 不读磁盘(list_active 是热路径),只填 id + active + title。
            info.title = entry->sm->current_title();
            info.title_source = entry->sm->current_title_source();
            info.turn_count = entry->sm->current_turn_count();
            info.last_token_usage = entry->sm->current_last_token_usage();
            info.session_token_usage = entry->sm->current_session_token_usage();
        }
        if (entry->perm) {
            info.permission_mode = PermissionManager::mode_name(entry->perm->mode());
        }
        auto model_state = entry->model_state;
        if (deps_.config) {
            mark_deleted_if_model_name_missing(*deps_.config, model_state);
        }
        info.provider = model_state.provider;
        info.model = model_state.model;
        info.model_name = model_state.name;
        info.context_window = model_state.context_window;
        info.model_deleted = model_state.deleted;
        out.push_back(std::move(info));
    }
    return out;
}

std::size_t SessionRegistry::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return entries_.size();
}

namespace {

// workspace cwd 判等:两侧 weakly_canonical 后比较(junction 教训 —— 本机
// C:\Users\x 与 N:\Users\x 是同一目录的两个形态,字符串直比会漏)。
bool same_workspace_cwd(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return a == b;
    std::error_code ec;
    auto ca = std::filesystem::weakly_canonical(path_from_utf8(a), ec);
    if (ec) return a == b;
    auto cb = std::filesystem::weakly_canonical(path_from_utf8(b), ec);
    if (ec) return a == b;
    return ca == cb;
}

} // namespace

bool SessionRegistry::any_busy_in_cwd(const std::string& cwd) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& [id, entry] : entries_) {
        if (!entry || !entry->loop) continue;
        if (entry->loop->is_busy() && same_workspace_cwd(entry->cwd, cwd)) {
            return true;
        }
    }
    return false;
}

void SessionRegistry::invalidate_git_snapshots_in_cwd(const std::string& cwd) {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& [id, entry] : entries_) {
        if (!entry || !entry->loop) continue;
        if (same_workspace_cwd(entry->cwd, cwd)) {
            entry->loop->invalidate_git_snapshot();
        }
    }
}

SessionRegistry::WebWorktreeResult SessionRegistry::enter_worktree_for_web(
    const std::string& id, const std::string& base_branch) {
    WebWorktreeResult result;

    auto entry = acquire(id);
    if (!entry || !entry->loop || !entry->sm) {
        result.http_status = 404;
        result.error = "unknown session";
        return result;
    }
    if (entry->loop->is_busy()) {
        result.http_status = 409;
        result.error = "session busy";
        return result;
    }
    // 仅"尚无消息"的新会话允许:该字段是首条消息的伴随意图,已开聊的
    // 会话中途切 worktree 归 EnterWorktree 工具管,UI 不提供第二条路径。
    if (!entry->loop->messages().empty()) {
        result.http_status = 400;
        result.error = "session already has messages";
        return result;
    }
    if (entry->sm->active_worktree().active()) {
        result.http_status = 400;
        result.error = "session is already in a worktree";
        return result;
    }
    if (!base_branch.empty() && !gitinfo::is_safe_ref_name(base_branch)) {
        result.http_status = 400;
        result.error = "invalid base branch name";
        return result;
    }

    const std::string session_cwd = entry->cwd.empty() ? deps_.cwd : entry->cwd;
    const std::string repo_root = worktree::find_canonical_git_root(session_cwd);
    if (repo_root.empty()) {
        result.http_status = 400;
        result.error = "workspace is not a git repository";
        return result;
    }

    // 命名用完整会话 id(YYYYMMDD-HHMMSS-xxxx,24 字符 < 64 上限):此刻
    // 会话还没有标题,id 稳定且可反查。不能只取前缀 —— id 前 8 位是日期,
    // 同一天创建的会话会 slug 撞车,第二个会话 fast-resume 复用第一个的
    // worktree(集成测试实际踩到)。
    const std::string slug = "ses-" + id;

    worktree::WorktreeCreateOptions options;
    options.base_branch = base_branch;
    if (deps_.config) options.sparse_paths = deps_.config->worktree.sparse_paths;
    auto created = worktree::get_or_create_worktree(repo_root, slug, options);
    if (!created.ok) {
        result.http_status = 500;
        // 基线分支不存在是 4xx 语义(用户输入问题),其余按 500。
        if (created.error.find("does not exist") != std::string::npos) {
            result.http_status = 400;
        }
        result.error = created.error;
        return result;
    }
    if (!created.existed) {
        worktree::PostCreationOptions post;
        if (deps_.config) {
            post.symlink_directories = deps_.config->worktree.symlink_directories;
        }
        worktree::perform_post_creation_setup(repo_root, created.worktree_path, post);
    }

    WorktreeSessionInfo info;
    info.original_cwd = session_cwd;
    info.worktree_path = created.worktree_path;
    info.worktree_name = slug;
    info.worktree_branch = created.worktree_branch;
    info.original_head_commit = created.head_commit;
    entry->sm->set_active_worktree(info);
    // set_cwd 同时失效 gitStatus 快照;entry->cwd(workspace 归属 / 会话
    // 存储位置)有意不动 —— 与 EnterWorktree 工具同语义。
    entry->loop->set_cwd(created.worktree_path);

    result.ok = true;
    result.http_status = 200;
    result.worktree_path = created.worktree_path;
    result.worktree_branch = created.worktree_branch;
    return result;
}

} // namespace acecode
