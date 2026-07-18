#include "loop_scheduler.hpp"

#include "../config/config.hpp"
#include "../session/session_registry.hpp"
#include "../utils/logger.hpp"
#include "../utils/utf8_path.hpp"
#include "../utils/uuid.hpp"
#include "../worktree/worktree_manager.hpp"

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <utility>
#include <vector>

namespace acecode::loop {
namespace {

std::int64_t system_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string event_error(const SessionEvent& event) {
    if (!event.payload.is_object()) return "session_error";
    for (const char* key : {"reason", "error", "message"}) {
        auto it = event.payload.find(key);
        if (it != event.payload.end() && it->is_string() && !it->get<std::string>().empty()) {
            return it->get<std::string>();
        }
    }
    return "session_error";
}

} // namespace

struct LoopScheduler::CallbackState {
    std::mutex mu;
    bool active = true;
    LoopStore* store = nullptr;
    SessionClient* client = nullptr;
    LoopScheduler::Clock clock;
    std::unordered_map<std::string, SessionClient::SubscriptionId> subscriptions;
    std::unordered_map<std::string, LoopEventState> events;
};

std::string build_loop_system_context(const LoopDefinition& loop,
                                      const LoopRun& run,
                                      const std::string& execution_root,
                                      const std::string& worktree_branch) {
    std::ostringstream out;
    out << "You are executing daemon-owned LOOP '" << loop.name << "' (loop_id="
        << loop.id << ", run_id=" << run.id << ").\n"
        << "Complete the coding task autonomously inside the execution root: "
        << (execution_root.empty() ? "the daemon no-workspace root" : execution_root) << ".\n";
    if (loop.permission_mode == "yolo") {
        out << "This LOOP uses Yolo mode. Read access outside the execution root is allowed, "
               "but every write must remain inside the execution root. AskUserQuestion is "
               "answered automatically; choose the best option and continue.\n";
    } else {
        out << "This LOOP uses Default mode. Permission requests and AskUserQuestion must wait "
               "for the user exactly as in an ordinary Default session.\n";
    }
    if (!worktree_branch.empty()) {
        out << "You are working in isolated Git worktree branch '" << worktree_branch << "'. "
               "Do not merge, rebase, push, or remove the worktree. When finished, summarize "
               "the changes and ask in your normal final assistant message whether the user "
               "wants to merge them into the main branch. Do not use AskUserQuestion for the "
               "merge decision.\n";
    }
    out << "Do not bypass the write boundary through shell scripts, child processes, symlinks, "
           "or indirect tools.";
    return out.str();
}

bool should_create_loop_worktree(const LoopDefinition& loop, bool git_workspace) {
    return loop.use_worktree && !loop.workspace_cwd.empty() && git_workspace;
}

std::optional<RunStatus> apply_loop_session_event(const SessionEvent& event,
                                                  LoopEventState& state,
                                                  std::string& reason) {
    reason.clear();
    if (event.kind == SessionEventKind::PermissionRequest ||
        event.kind == SessionEventKind::QuestionRequest) {
        state.waiting_user = true;
        return RunStatus::WaitingUser;
    }
    if (event.kind == SessionEventKind::AgentProgress && event.payload.is_object() &&
        event.payload.value("phase", std::string{}) == "permission_waiting") {
        state.waiting_user = true;
        return RunStatus::WaitingUser;
    }
    if (event.kind == SessionEventKind::BusyChanged && event.payload.is_object() &&
        event.payload.value("busy", false) && state.waiting_user) {
        state.waiting_user = false;
        return RunStatus::Running;
    }
    if (event.kind == SessionEventKind::Error) {
        state.error = event_error(event);
        return std::nullopt;
    }
    if (event.kind == SessionEventKind::Done) {
        if (!state.error.empty()) {
            reason = state.error;
            return RunStatus::Failed;
        }
        return RunStatus::Completed;
    }
    return std::nullopt;
}

LoopScheduler::LoopScheduler(LoopStore& store,
                             SessionRegistry& registry,
                             SessionClient& client,
                             const AppConfig& config,
                             Clock clock)
    : store_(store), registry_(registry), client_(client), config_(config),
      clock_(clock ? std::move(clock) : Clock(system_now_ms)),
      owner_id_(generate_uuid()), callbacks_(std::make_shared<CallbackState>()) {
    callbacks_->store = &store_;
    callbacks_->client = &client_;
    callbacks_->clock = clock_;
}

LoopScheduler::~LoopScheduler() { stop(); }

bool LoopScheduler::start(StoreError* error) {
    if (running_.exchange(true)) return true;
    stop_requested_.store(false);
    {
        std::lock_guard<std::mutex> lock(callbacks_->mu);
        callbacks_->active = true;
    }
    if (!store_.record_offline_missed(clock_(), owner_id_, error)) {
        running_.store(false);
        return false;
    }
    worker_ = std::thread([this] { run(); });
    return true;
}

void LoopScheduler::stop() {
    if (!running_.exchange(false)) return;
    stop_requested_.store(true);
    wait_cv_.notify_all();
    if (worker_.joinable()) worker_.join();

    std::vector<std::pair<std::string, SessionClient::SubscriptionId>> subscriptions;
    {
        std::lock_guard<std::mutex> lock(callbacks_->mu);
        callbacks_->active = false;
        for (const auto& item : callbacks_->subscriptions) subscriptions.push_back(item);
        callbacks_->subscriptions.clear();
        callbacks_->events.clear();
    }
    for (const auto& [session_id, id] : subscriptions) {
        client_.unsubscribe(session_id, id);
        client_.abort(session_id);
    }
    StoreError ignored;
    store_.interrupt_owner_runs(owner_id_, clock_(), &ignored);
}

void LoopScheduler::notify_changed() { wait_cv_.notify_all(); }

bool LoopScheduler::model_exists(const std::string& name) const {
    for (const auto& model : config_.saved_models) {
        if (model.name == name) return true;
    }
    return false;
}

void LoopScheduler::fail_run(const LoopDefinition& loop,
                             const LoopRun& run,
                             const std::string& reason,
                             bool disable_loop,
                             const std::string& session_id) {
    StoreError error;
    store_.update_run_state(run.id, RunStatus::Failed, clock_(), reason, session_id,
                            {}, {}, &error);
    if (disable_loop) store_.set_loop_enabled(loop.id, false, clock_(), &error);
    LOG_WARN("[loop] run " + run.id + " failed: " + reason);
}

void LoopScheduler::launch(const LoopDefinition& loop, const LoopRun& run) {
    if (!model_exists(loop.model_name)) {
        fail_run(loop, run, "model_unavailable", true);
        return;
    }
    const bool no_workspace = loop.workspace_cwd.empty();
    if (!no_workspace) {
        std::error_code ec;
        if (!std::filesystem::is_directory(path_from_utf8(loop.workspace_cwd), ec) || ec) {
            fail_run(loop, run, "workspace_unavailable", false);
            return;
        }
    }

    SessionOptions opts;
    opts.cwd = loop.workspace_cwd;
    opts.workspace_hash = loop.workspace_hash;
    opts.no_workspace = no_workspace;
    opts.model_name = loop.model_name;
    opts.permission_mode = loop.permission_mode;
    opts.loop_execution = true;
    opts.loop_id = loop.id;
    opts.loop_run_id = run.id;
    opts.loop_system_context = build_loop_system_context(loop, run, loop.workspace_cwd, {});

    std::string session_id;
    try {
        session_id = client_.create_session(opts);
    } catch (const std::exception& e) {
        fail_run(loop, run, std::string("session_create_failed: ") + e.what());
        return;
    }
    if (session_id.empty()) {
        fail_run(loop, run, "session_create_failed");
        return;
    }

    std::string execution_root = loop.workspace_cwd;
    std::string worktree_path;
    std::string worktree_branch;
    bool git_workspace = false;
    if (loop.use_worktree && !no_workspace) {
        // A .git marker without an installed/working git executable is treated
        // as a non-Git workspace. Once an opted-in Git workspace is detected,
        // worktree creation remains fail-closed.
        const auto git_probe =
            worktree::run_git({"rev-parse", "--is-inside-work-tree"}, loop.workspace_cwd);
        git_workspace =
            git_probe.ok() &&
            !worktree::find_canonical_git_root(loop.workspace_cwd).empty();
    }
    if (should_create_loop_worktree(loop, git_workspace)) {
        auto worktree = registry_.enter_worktree_for_web(session_id, {});
        if (!worktree.ok) {
            fail_run(loop, run, "worktree_create_failed: " + worktree.error, false, session_id);
            return;
        }
        worktree_path = worktree.worktree_path;
        worktree_branch = worktree.worktree_branch;
        execution_root = worktree_path;
        auto entry = registry_.acquire(session_id);
        if (entry && entry->loop) {
            entry->loop->set_loop_execution_policy({
                true, build_loop_system_context(loop, run, execution_root, worktree_branch)});
        }
    }

    StoreError error;
    if (!store_.update_run_state(run.id, RunStatus::Running, clock_(), {}, session_id,
                                 worktree_path, worktree_branch, &error)) {
        client_.abort(session_id);
        return;
    }

    const auto state = callbacks_;
    auto subscription = client_.subscribe(session_id,
        [state, session_id, run_id = run.id](const SessionEvent& event) {
            SessionClient::SubscriptionId unsubscribe_id = 0;
            RunStatus status = RunStatus::Running;
            std::string reason;
            bool persist = false;
            bool terminal = false;
            {
                std::lock_guard<std::mutex> lock(state->mu);
                if (!state->active) return;
                auto& event_state = state->events[session_id];
                auto next = apply_loop_session_event(event, event_state, reason);
                if (!next) return;
                status = *next;
                persist = true;
                terminal = status == RunStatus::Completed || status == RunStatus::Failed;
                if (terminal) {
                    auto it = state->subscriptions.find(session_id);
                    if (it != state->subscriptions.end()) {
                        unsubscribe_id = it->second;
                        state->subscriptions.erase(it);
                    }
                    state->events.erase(session_id);
                }
            }
            if (persist) {
                StoreError ignored;
                state->store->update_run_state(run_id, status, state->clock(), reason,
                                               session_id, {}, {}, &ignored);
            }
            if (terminal && unsubscribe_id != 0) {
                state->client->unsubscribe(session_id, unsubscribe_id);
            }
        });
    if (subscription == 0) {
        fail_run(loop, run, "session_subscribe_failed", false, session_id);
        client_.abort(session_id);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(state->mu);
        if (!state->active) {
            client_.unsubscribe(session_id, subscription);
            return;
        }
        state->subscriptions[session_id] = subscription;
        state->events.try_emplace(session_id);
    }
    if (!client_.send_input(session_id, loop.prompt)) {
        client_.unsubscribe(session_id, subscription);
        {
            std::lock_guard<std::mutex> lock(state->mu);
            state->subscriptions.erase(session_id);
            state->events.erase(session_id);
        }
        fail_run(loop, run, "session_send_failed", false, session_id);
    }
}

void LoopScheduler::run() {
    constexpr std::int64_t kLiveWakeGraceMs = 5000;
    while (!stop_requested_.load()) {
        StoreError error;
        const auto now = clock_();
        // If the process stayed alive through sleep, aggregate occurrences that
        // are clearly stale. A one-minute grace avoids classifying normal wake
        // jitter as missed.
        store_.record_offline_missed(now - kLiveWakeGraceMs, owner_id_, &error);
        auto claim = store_.claim_due(now, owner_id_, &error);
        if (claim.disposition == ClaimDisposition::Claimed && claim.loop && claim.run) {
            launch(*claim.loop, *claim.run);
            continue;
        }
        if (claim.disposition == ClaimDisposition::MissedWorkspaceBusy) continue;

        auto next = store_.earliest_next_run_at(&error);
        std::unique_lock<std::mutex> lock(wait_mu_);
        if (stop_requested_.load()) break;
        if (!next) {
            wait_cv_.wait(lock);
        } else {
            const auto delay = std::max<std::int64_t>(0, *next - clock_());
            wait_cv_.wait_for(lock, std::chrono::milliseconds(delay));
        }
    }
}

} // namespace acecode::loop
