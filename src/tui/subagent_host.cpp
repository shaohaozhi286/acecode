#include "subagent_host.hpp"

#include "../agent_loop.hpp"
#include "../session/session_manager.hpp"
#include "../session/session_storage.hpp"
#include "../session/session_user_message_search.hpp"
#include "../utils/logger.hpp"

#include <algorithm>

namespace acecode::tui {

SubagentHost::SubagentHost(Deps deps)
    : registry_(deps.registry_deps), client_(registry_), deps_(std::move(deps)) {}

void SubagentHost::on_spawned(const std::string& child_id,
                              const std::string& prompt) {
    if (child_id.empty()) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        running_.push_back({child_id, "", prompt,
                            std::chrono::steady_clock::now()});
        publish_locked();
    }
    // 订阅子会话事件流。回调在子会话 loop 线程触发;不显式退订 ——
    // dispatcher 生命周期 = SessionEntry,destroy/clear 时一起消亡。
    //
    // 竞态兜底:on_spawn 在 send_input 之后被调用,极快的 turn(stub /
    // 瞬时失败)可能在订阅建立前就发完 BusyChanged(false)。订阅后补查一次:
    // 已空闲且已有 assistant 答复 → 视为已结束,直接移除。
    client_.subscribe(child_id, [this, child_id](const SessionEvent& evt) {
        switch (evt.kind) {
            case SessionEventKind::BusyChanged:
                // 右侧列只显示运行中:本轮结束即移除(用户决策)。
                if (!evt.payload.value("busy", false)) {
                    remove_task(child_id);
                }
                break;
            case SessionEventKind::SessionUpdated: {
                const std::string title =
                    evt.payload.value("title", std::string{});
                if (!title.empty()) update_title(child_id, title);
                break;
            }
            case SessionEventKind::PermissionRequest:
                if (deps_.on_permission_request) {
                    deps_.on_permission_request(child_id, title_for(child_id),
                                                evt.payload);
                }
                break;
            default:
                break;
        }
    });
    if (auto entry = registry_.acquire(child_id)) {
        if (entry->loop && !entry->loop->is_busy() && entry->sm) {
            for (const auto& msg : entry->sm->load_active_messages()) {
                if (msg.role == "assistant" && !msg.content.empty()) {
                    remove_task(child_id);
                    break;
                }
            }
        }
    }
}

std::vector<SubagentTaskSnapshot> SubagentHost::running_tasks() const {
    std::lock_guard<std::mutex> lk(mu_);
    return running_;
}

std::vector<SubagentHost::TaskListEntry>
SubagentHost::list_tasks(const std::string& project_dir) const {
    const std::string parent =
        deps_.parent_session_id ? deps_.parent_session_id() : std::string{};
    std::vector<TaskListEntry> out;
    std::vector<std::string> running_ids;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& t : running_) {
            out.push_back({t.id, t.title.empty() ? t.prompt : t.title,
                           /*running=*/true, 0});
            running_ids.push_back(t.id);
        }
    }
    if (!parent.empty() && !project_dir.empty()) {
        for (const auto& meta : SessionStorage::list_sessions(project_dir)) {
            if (meta.parent_session_id != parent) continue;
            if (std::find(running_ids.begin(), running_ids.end(), meta.id) !=
                running_ids.end()) {
                continue;
            }
            out.push_back({meta.id,
                           meta.title.empty() ? meta.summary : meta.title,
                           /*running=*/false, meta.message_count});
        }
    }
    return out;
}

bool SubagentHost::abort_task(const std::string& id) {
    auto entry = registry_.acquire(id);
    if (!entry || !entry->loop) return false;
    entry->loop->abort();
    // BusyChanged(false) 事件随后到达并移除任务;这里不提前动列表,
    // 避免「中止请求发出但子会话还在收尾」期间右侧列消失误导用户。
    return true;
}

int SubagentHost::clear_settled(const std::string& project_dir) {
    const std::string parent =
        deps_.parent_session_id ? deps_.parent_session_id() : std::string{};
    if (parent.empty() || project_dir.empty()) return 0;
    std::vector<std::string> running_ids;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& t : running_) running_ids.push_back(t.id);
    }
    int removed = 0;
    SessionUserMessageIndex search_index(project_dir);
    for (const auto& meta : SessionStorage::list_sessions(project_dir)) {
        if (meta.parent_session_id != parent) continue;
        if (std::find(running_ids.begin(), running_ids.end(), meta.id) !=
            running_ids.end()) {
            continue;
        }
        registry_.destroy(meta.id);  // 不在 registry 时是 no-op
        SessionStorage::purge_session_files(project_dir, meta.id);
        {
            // 与 Web 端 purge 一致:永久删除必须连用户消息搜索索引一起清,
            // 否则子会话的用户输入全文残留在索引数据库。
            std::string index_error;
            if (!search_index.remove_session(meta.id, &index_error)) {
                LOG_WARN("[subagent] purge failed to remove search index for " +
                         meta.id + ": " + index_error);
            }
        }
        ++removed;
        LOG_INFO("[subagent] purged settled task " + meta.id);
    }
    return removed;
}

void SubagentHost::respond_permission(const std::string& session_id,
                                      const std::string& request_id,
                                      const std::string& choice) {
    PermissionDecision decision;
    decision.request_id = request_id;
    decision.choice =
        parse_permission_choice(choice).value_or(PermissionDecisionChoice::Deny);
    client_.respond_permission(session_id, decision);
}

void SubagentHost::publish_locked() {
    if (deps_.publish_tasks) deps_.publish_tasks(running_);
}

void SubagentHost::remove_task(const std::string& id) {
    std::lock_guard<std::mutex> lk(mu_);
    const auto before = running_.size();
    running_.erase(std::remove_if(running_.begin(), running_.end(),
                                  [&](const auto& t) { return t.id == id; }),
                   running_.end());
    if (running_.size() != before) publish_locked();
}

void SubagentHost::update_title(const std::string& id,
                                const std::string& title) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& t : running_) {
        if (t.id == id && t.title != title) {
            t.title = title;
            publish_locked();
            return;
        }
    }
}

std::string SubagentHost::title_for(const std::string& id) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& t : running_) {
        if (t.id == id) return t.title.empty() ? t.prompt : t.title;
    }
    return id;
}

} // namespace acecode::tui
