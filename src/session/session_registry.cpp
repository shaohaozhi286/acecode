#include "session_registry.hpp"

#include "session_storage.hpp"
#include "../utils/logger.hpp"

#include <utility>

namespace acecode {

SessionRegistry::SessionRegistry(SessionRegistryDeps deps)
    : deps_(std::move(deps)) {}

SessionRegistry::~SessionRegistry() {
    // entries_ 析构会触发每个 SessionEntry 析构 → AgentLoop::shutdown 等等
    // worker thread join。锁不需要 — 此时没人再调 lookup/destroy(daemon
    // 退出路径)。
}

std::string SessionRegistry::create(const SessionOptions& opts) {
    // 用 SessionStorage 的 id 生成器 — 与 jsonl 文件名格式一致,后续 list
    // 合并磁盘历史时不会冲突。
    std::string id = SessionStorage::generate_session_id();

    auto entry = std::make_unique<SessionEntry>();
    entry->id = id;

    // SessionManager
    entry->sm = std::make_unique<SessionManager>();
    entry->sm->start_session(deps_.cwd,
                              "daemon",  // provider/model 占位 — 后续 swap
                              opts.model_name.empty() ? "default" : opts.model_name);

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
    entry->loop = std::make_unique<AgentLoop>(
        deps_.provider_accessor,
        *deps_.tools,
        std::move(empty_cb),
        deps_.cwd,
        *entry->perm);

    if (deps_.config) {
        entry->loop->set_context_window(deps_.config->context_window);
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

    {
        std::lock_guard<std::mutex> lk(mu_);
        entries_.emplace(id, std::move(entry));
    }
    LOG_INFO("[registry] created session " + id);
    return id;
}

SessionEntry* SessionRegistry::lookup(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = entries_.find(id);
    return it == entries_.end() ? nullptr : it->second.get();
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
        info.active = true;
        if (entry->sm) {
            // SessionManager 没有公开的 created_at / updated_at 接口,从 meta
            // 拿:这里**可选**调 load_session_meta 走磁盘读,有 IO 成本。
            // v1 不读磁盘(list_active 是热路径),只填 id + active + title。
            info.title = entry->sm->current_title();
        }
        out.push_back(std::move(info));
    }
    return out;
}

std::size_t SessionRegistry::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return entries_.size();
}

} // namespace acecode
