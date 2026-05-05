#include "session_registry.hpp"

#include "session_rewind.hpp"
#include "session_storage.hpp"
#include "../provider/model_context_resolver.hpp"
#include "../provider/model_resolver.hpp"
#include "../provider/provider_factory.hpp"
#include "../utils/logger.hpp"
#include "../utils/cwd_hash.hpp"

#include <filesystem>
#include <optional>
#include <utility>

namespace acecode {

namespace {

bool is_llm_role(const std::string& role) {
    return role == "user" || role == "assistant" ||
           role == "system" || role == "tool";
}

std::pair<std::string, std::string>
current_provider_model(const SessionRegistryDeps& deps,
                       const std::string& fallback_model) {
    if (deps.provider_accessor) {
        auto provider = deps.provider_accessor();
        if (provider) {
            return {provider->name(), provider->model()};
        }
    }
    return {"daemon", fallback_model.empty() ? "default" : fallback_model};
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
    if (name == "(legacy)") return synth_legacy_entry(cfg);
    if (const auto* entry = find_profile_by_name(cfg, name)) return *entry;
    return std::nullopt;
}

AppConfig config_for_profile_context(const AppConfig& cfg,
                                     const ModelProfile& profile) {
    AppConfig context_cfg = cfg;
    context_cfg.provider = profile.provider;
    if (profile.provider == "openai") {
        context_cfg.openai.base_url = profile.base_url;
        context_cfg.openai.api_key = profile.api_key;
        context_cfg.openai.model = profile.model;
        context_cfg.openai.models_dev_provider_id = profile.models_dev_provider_id;
    } else {
        context_cfg.copilot.model = profile.model;
    }
    return context_cfg;
}

SessionModelState state_from_profile(const AppConfig& cfg,
                                     const ModelProfile& profile) {
    auto context_cfg = config_for_profile_context(cfg, profile);
    SessionModelState state;
    state.name = profile.name;
    state.provider = profile.provider;
    state.model = profile.model;
    state.context_window = resolve_model_context_window(
        context_cfg, profile.provider, profile.model, cfg.context_window);
    state.is_legacy = profile.name == "(legacy)";
    return state;
}

struct ResolvedSessionModel {
    SessionModelState state;
    std::shared_ptr<LlmProvider> provider;
};

ResolvedSessionModel resolve_from_profile(const AppConfig& cfg,
                                          const ModelProfile& profile) {
    ResolvedSessionModel resolved;
    resolved.state = state_from_profile(cfg, profile);
    resolved.provider = create_provider_from_entry(profile);
    return resolved;
}

ResolvedSessionModel resolve_session_model(const SessionRegistryDeps& deps,
                                           const SessionOptions& opts,
                                           const SessionMeta* resumed_meta) {
    if (deps.config) {
        ModelProfile profile;
        if (!opts.model_name.empty()) {
            auto explicit_match = explicit_profile(*deps.config, opts.model_name);
            if (explicit_match.has_value()) {
                profile = *explicit_match;
            } else {
                LOG_WARN("[registry] requested model preset '" + opts.model_name +
                         "' not found; falling back to default/legacy");
                profile = resolve_effective_model(*deps.config, std::nullopt, std::nullopt);
            }
        } else if (resumed_meta) {
            profile = resolve_effective_model(
                *deps.config, std::nullopt, std::optional<SessionMeta>{*resumed_meta});
        } else {
            profile = resolve_effective_model(*deps.config, std::nullopt, std::nullopt);
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
    resolved.state.is_legacy = opts.model_name == "(legacy)";
    return resolved;
}

SessionOptions with_resolved_workspace(const SessionRegistryDeps& deps,
                                       const SessionOptions& in) {
    SessionOptions out = in;
    if (out.cwd.empty()) {
        out.cwd = deps.cwd;
    }
    if (out.workspace_hash.empty() && !out.cwd.empty()) {
        out.workspace_hash = compute_cwd_hash(out.cwd);
    }
    return out;
}

} // namespace

SessionRegistry::SessionRegistry(SessionRegistryDeps deps)
    : deps_(std::move(deps)) {}

SessionRegistry::~SessionRegistry() {
    // entries_ 析构会触发每个 SessionEntry 析构 → AgentLoop::shutdown 等等
    // worker thread join。锁不需要 — 此时没人再调 lookup/destroy(daemon
    // 退出路径)。
}

std::string SessionRegistry::create(const SessionOptions& opts) {
    std::string id = SessionStorage::generate_session_id();
    SessionOptions resolved = with_resolved_workspace(deps_, opts);

    auto entry = make_entry_locked(id, resolved, nullptr);
    {
        std::lock_guard<std::mutex> lk(mu_);
        entries_.emplace(id, std::move(entry));
    }
    LOG_INFO("[registry] created session " + id);
    return id;
}

std::unique_ptr<SessionEntry>
SessionRegistry::make_entry_locked(const std::string& id,
                                   const SessionOptions& opts,
                                   const SessionMeta* resumed_meta) {
    auto resolved_model = resolve_session_model(deps_, opts, resumed_meta);

    auto entry = std::make_unique<SessionEntry>();
    entry->id = id;
    entry->cwd = opts.cwd.empty() ? deps_.cwd : opts.cwd;
    entry->workspace_hash = opts.workspace_hash.empty()
        ? compute_cwd_hash(entry->cwd)
        : opts.workspace_hash;
    entry->provider = resolved_model.state.provider;
    entry->model = resolved_model.state.model;
    entry->model_state = resolved_model.state;
    entry->provider_slot = std::make_shared<SessionEntry::ProviderSlot>();
    entry->provider_slot->provider = std::move(resolved_model.provider);

    // SessionManager
    entry->sm = std::make_unique<SessionManager>();
    entry->sm->start_session(entry->cwd,
                             entry->model_state.provider,
                             entry->model_state.model,
                             id,
                             entry->model_state.name);

    // PermissionManager: 复制 mode + dangerous flag,rules 由调用方在初始化
    // template_permissions 时设好。session_allowed_ 不复制,各 session 独立。
    entry->perm = std::make_unique<PermissionManager>();
    if (deps_.template_permissions) {
        entry->perm->set_mode(deps_.template_permissions->mode());
        entry->perm->set_dangerous(deps_.template_permissions->is_dangerous());
        // 注意: rules 当前没有 copy 接口 — v1 暂不复制 rules,daemon 路径
        // 自己装(后续 Section 9 落 HTTP 时一起补)。TUI 路径不受影响。
    }

    // AgentLoop: 给一个空 callbacks(daemon 全走 events_)
    AgentCallbacks empty_cb;
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

    if (deps_.config) {
        entry->loop->set_context_window(entry->model_state.context_window > 0
            ? entry->model_state.context_window
            : deps_.config->context_window);
        entry->loop->set_agent_loop_config(deps_.config->agent_loop);
    }
    entry->loop->set_session_manager(entry->sm.get());
    entry->loop->set_skill_registry(deps_.skill_registry);
    entry->loop->set_memory_registry(deps_.memory_registry);
    entry->loop->set_memory_config(deps_.memory_cfg);
    entry->loop->set_project_instructions_config(deps_.project_instructions_cfg);

    // PermissionPrompter: 异步 — 触发 PermissionRequest 事件,等浏览器 decision
    auto prompter = std::make_unique<AsyncPrompter>(entry->loop->events());
    entry->prompter = prompter.get();
    entry->loop->set_permission_prompter(std::move(prompter));

    // AskUserQuestionPrompter: 异步 — 触发 QuestionRequest 事件,等浏览器
    // question_answer 回流。AgentLoop 在每次工具执行时把 prompter 包成
    // ToolContext::ask_user_questions 回调注入(set_ask_question_prompter)。
    entry->ask_prompter = std::make_unique<AskUserQuestionPrompter>(entry->loop->events());
    entry->loop->set_ask_question_prompter(entry->ask_prompter.get());

    // 可选 auto_start: 立刻 submit initial_user_message
    if (opts.auto_start && !opts.initial_user_message.empty()) {
        entry->loop->submit(opts.initial_user_message);
    }

    return entry;
}

void SessionRegistry::restore_loop_history(
    SessionEntry& entry,
    const std::vector<ChatMessage>& messages) const {
    if (!entry.loop) return;
    entry.loop->clear_messages();

    for (std::size_t i = 0; i < messages.size(); ++i) {
        const auto& msg = messages[i];
        if (is_file_checkpoint_message(msg)) {
            continue;
        }

        const bool is_shell_user =
            (msg.role == "user" && !msg.content.empty() && msg.content[0] == '!');
        const bool next_is_result =
            (i + 1 < messages.size() && messages[i + 1].role == "tool_result");
        if (is_shell_user && next_is_result) {
            entry.loop->inject_shell_turn(msg.content.substr(1),
                                          messages[i + 1].content,
                                          "",
                                          0);
            ++i;
            continue;
        }

        if (is_llm_role(msg.role)) {
            entry.loop->push_message(msg);
        }
    }
}

bool SessionRegistry::resume(const std::string& id, const SessionOptions& opts) {
    if (id.empty()) return false;
    SessionOptions resolved = with_resolved_workspace(deps_, opts);

    std::lock_guard<std::mutex> lk(mu_);
    if (entries_.find(id) != entries_.end()) {
        return true;
    }

    SessionManager meta_reader;
    auto [provider, model] = current_provider_model(deps_, "");
    meta_reader.start_session(resolved.cwd, provider, model);
    SessionMeta meta = meta_reader.load_session_meta(id);

    if (meta.id.empty()) {
        return false;
    }

    SessionOptions entry_opts;
    entry_opts.cwd = resolved.cwd;
    entry_opts.workspace_hash = resolved.workspace_hash;
    auto entry = make_entry_locked(id, entry_opts, &meta);
    auto messages = entry->sm->resume_session(id);
    restore_loop_history(*entry, messages);

    entries_.emplace(id, std::move(entry));
    LOG_INFO("[registry] resumed session " + id);
    return true;
}

SessionEntry* SessionRegistry::lookup(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = entries_.find(id);
    return it == entries_.end() ? nullptr : it->second.get();
}

std::optional<SessionModelState>
SessionRegistry::current_model_state(const std::string& id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = entries_.find(id);
    if (it == entries_.end() || !it->second) return std::nullopt;
    return it->second->model_state;
}

bool SessionRegistry::switch_model(const std::string& id,
                                   const ModelProfile& profile,
                                   SessionModelState* out,
                                   std::string* error) {
    if (!deps_.config) {
        if (error) *error = "config unavailable";
        return false;
    }

    ResolvedSessionModel resolved;
    try {
        resolved = resolve_from_profile(*deps_.config, profile);
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
    if (!resolved.provider) {
        if (error) *error = "provider unavailable";
        return false;
    }

    std::lock_guard<std::mutex> lk(mu_);
    auto it = entries_.find(id);
    if (it == entries_.end() || !it->second) {
        if (error) *error = "session not found";
        return false;
    }

    auto& entry = *it->second;
    if (!entry.provider_slot) {
        entry.provider_slot = std::make_shared<SessionEntry::ProviderSlot>();
    }
    {
        std::lock_guard<std::mutex> provider_lk(entry.provider_slot->mu);
        entry.provider_slot->provider = std::move(resolved.provider);
    }

    entry.model_state = resolved.state;
    entry.provider = resolved.state.provider;
    entry.model = resolved.state.model;
    if (entry.loop && resolved.state.context_window > 0) {
        entry.loop->set_context_window(resolved.state.context_window);
    }
    if (entry.sm) {
        entry.sm->set_active_provider(resolved.state.provider,
                                      resolved.state.model,
                                      resolved.state.name);
    }
    if (out) *out = resolved.state;
    return true;
}

std::optional<SessionModelState>
SessionRegistry::model_state_from_meta(const SessionMeta& meta) const {
    if (meta.id.empty() || !deps_.config) return std::nullopt;
    auto profile = resolve_effective_model(
        *deps_.config, std::nullopt, std::optional<SessionMeta>{meta});
    return state_from_profile(*deps_.config, profile);
}

void SessionRegistry::destroy(const std::string& id) {
    std::unique_ptr<SessionEntry> moved;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = entries_.find(id);
        if (it == entries_.end()) return;
        moved = std::move(it->second);
        entries_.erase(it);
    }
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
        if (entry->loop) info.busy = entry->loop->is_busy();
        if (entry->sm) {
            // SessionManager 没有公开的 created_at / updated_at 接口,从 meta
            // 拿:这里**可选**调 load_session_meta 走磁盘读,有 IO 成本。
            // v1 不读磁盘(list_active 是热路径),只填 id + active + title。
            info.title = entry->sm->current_title();
        }
        info.provider = entry->provider;
        info.model = entry->model;
        info.model_name = entry->model_state.name;
        info.context_window = entry->model_state.context_window;
        info.model_is_legacy = entry->model_state.is_legacy;
        out.push_back(std::move(info));
    }
    return out;
}

std::size_t SessionRegistry::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return entries_.size();
}

} // namespace acecode
