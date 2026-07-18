#include "loop/loop_scheduler.hpp"

#include "config/config.hpp"
#include "permissions.hpp"
#include "session/session_registry.hpp"
#include "tool/tool_executor.hpp"
#include "utils/uuid.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <thread>

namespace acecode::loop {
namespace {

LoopDefinition sample_loop(std::string permission = "yolo") {
    LoopDefinition loop;
    loop.id = "loop-1";
    loop.name = "Review changes";
    loop.prompt = "Review the repository";
    loop.permission_mode = std::move(permission);
    return loop;
}

LoopRun sample_run() {
    LoopRun run;
    run.id = "run-1";
    run.loop_id = "loop-1";
    return run;
}

class FakeLoopSessionClient final : public SessionClient {
public:
    std::string create_session(const SessionOptions& opts) override {
        created_options = opts;
        created = true;
        return "visible-session";
    }
    bool resume_session(const std::string&, const SessionOptions&) override { return false; }
    std::vector<SessionInfo> list_sessions() override { return {}; }
    void destroy_session(const std::string&) override {}
    SubscriptionId subscribe(const std::string& session_id,
                             EventListener listener,
                             std::uint64_t) override {
        subscribed_session = session_id;
        listener_ = std::move(listener);
        return 7;
    }
    void unsubscribe(const std::string&, SubscriptionId) override { unsubscribed = true; }
    bool send_input(const std::string& session_id, const std::string& text) override {
        sent_after_subscribe = listener_ && subscribed_session == session_id;
        sent_text = text;
        if (listener_) listener_(SessionEvent{SessionEventKind::Done});
        return true;
    }
    BuiltinCommandResult execute_builtin_command(
        const std::string&, const BuiltinCommandRequest&) override { return {}; }
    void respond_permission(const std::string&, const PermissionDecision&) override {}
    void respond_question(const std::string&, const std::string&,
                          const AskUserQuestionResponse&) override {}
    void abort(const std::string&) override { aborted = true; }

    SessionOptions created_options;
    EventListener listener_;
    std::string subscribed_session;
    std::string sent_text;
    bool created = false;
    bool sent_after_subscribe = false;
    bool unsubscribed = false;
    bool aborted = false;
};

TEST(LoopScheduler, SystemContextContainsYoloBoundaryAndWorktreeHandoff) {
    const auto text = build_loop_system_context(
        sample_loop(), sample_run(), "C:/repo/.acecode/worktrees/ses-1", "worktree-ses-1");
    EXPECT_NE(text.find("LOOP 'Review changes'"), std::string::npos);
    EXPECT_NE(text.find("Read access outside"), std::string::npos);
    EXPECT_NE(text.find("every write must remain inside"), std::string::npos);
    EXPECT_NE(text.find("Do not merge, rebase, push, or remove"), std::string::npos);
    EXPECT_NE(text.find("normal final assistant message"), std::string::npos);
    EXPECT_NE(text.find("Do not use AskUserQuestion"), std::string::npos);
}

TEST(LoopScheduler, DefaultContextPreservesInteractiveQuestions) {
    const auto text = build_loop_system_context(
        sample_loop("default"), sample_run(), "C:/repo", {});
    EXPECT_NE(text.find("must wait for the user exactly as in an ordinary Default session"),
              std::string::npos);
    EXPECT_EQ(text.find("answered automatically"), std::string::npos);
}

TEST(LoopScheduler, WorktreeDecisionRequiresExplicitOptInAndGitWorkspace) {
    auto loop = sample_loop();
    loop.workspace_hash = "workspace";
    loop.workspace_cwd = "C:/repo";

    EXPECT_FALSE(should_create_loop_worktree(loop, true));
    loop.use_worktree = true;
    EXPECT_TRUE(should_create_loop_worktree(loop, true));
    EXPECT_FALSE(should_create_loop_worktree(loop, false));

    loop.workspace_hash.clear();
    loop.workspace_cwd.clear();
    EXPECT_FALSE(should_create_loop_worktree(loop, true));
}

TEST(LoopScheduler, EventProjectionTracksWaitingResumeAndCompletion) {
    LoopEventState state;
    std::string reason;
    SessionEvent question{SessionEventKind::QuestionRequest};
    ASSERT_EQ(apply_loop_session_event(question, state, reason), RunStatus::WaitingUser);
    EXPECT_TRUE(state.waiting_user);

    SessionEvent busy{SessionEventKind::BusyChanged};
    busy.payload = {{"busy", true}};
    ASSERT_EQ(apply_loop_session_event(busy, state, reason), RunStatus::Running);
    EXPECT_FALSE(state.waiting_user);

    SessionEvent done{SessionEventKind::Done};
    EXPECT_EQ(apply_loop_session_event(done, state, reason), RunStatus::Completed);
    EXPECT_TRUE(reason.empty());
}

TEST(LoopScheduler, ErrorBecomesFailureWhenTurnEnds) {
    LoopEventState state;
    std::string reason;
    SessionEvent error{SessionEventKind::Error};
    error.payload = {{"reason", "provider unavailable"}};
    EXPECT_FALSE(apply_loop_session_event(error, state, reason).has_value());

    SessionEvent done{SessionEventKind::Done};
    EXPECT_EQ(apply_loop_session_event(done, state, reason), RunStatus::Failed);
    EXPECT_EQ(reason, "provider unavailable");
}

TEST(LoopScheduler, DueRunCreatesVisibleSessionAndCapturesTerminalEvent) {
    const auto root = std::filesystem::temp_directory_path() /
        ("acecode-loop-scheduler-" + generate_uuid());
    std::filesystem::create_directories(root);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    } cleanup{root};

    AppConfig config;
    ModelProfile model;
    model.name = "model-a";
    model.provider = "openai";
    model.model = "stub";
    config.saved_models.push_back(model);
    ToolExecutor tools;
    PermissionManager permissions;
    SessionRegistryDeps deps;
    deps.tools = &tools;
    deps.cwd = root.string();
    deps.no_workspace_cache_root = (root / "no-workspace").string();
    deps.template_permissions = &permissions;
    SessionRegistry registry(std::move(deps));
    FakeLoopSessionClient client;
    LoopStore store(root / "loops.sqlite3");
    StoreError error;
    ASSERT_TRUE(store.initialize(&error)) << error.message;

    constexpr std::int64_t now = 2'000'000;
    auto loop = sample_loop();
    loop.model_name = "model-a";
    loop.prompt = "finish quickly";
    loop.workspace_hash = "workspace";
    loop.workspace_cwd = root.string();
    loop.use_worktree = false;
    loop.schedule.kind = ScheduleKind::Once;
    loop.schedule.once_at_ms = now;
    auto created = store.create_loop(loop, now - 1, &error);
    ASSERT_TRUE(created.has_value()) << error.message;

    LoopScheduler scheduler(store, registry, client, config, [=] { return now; });
    ASSERT_TRUE(scheduler.start(&error)) << error.message;
    bool terminal = false;
    LoopRun observed;
    for (int i = 0; i < 100 && !terminal; ++i) {
        auto runs = store.list_runs(created->id, 10, &error);
        if (!runs.empty()) {
            observed = runs.front();
            terminal = observed.status == RunStatus::Completed ||
                       observed.status == RunStatus::Failed;
        }
        if (!terminal) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    scheduler.stop();

    EXPECT_TRUE(terminal);
    EXPECT_EQ(observed.status, RunStatus::Completed);
    EXPECT_FALSE(observed.session_id.empty());
    EXPECT_TRUE(client.created);
    EXPECT_TRUE(client.sent_after_subscribe);
    EXPECT_TRUE(client.unsubscribed);
    EXPECT_EQ(client.sent_text, "finish quickly");
    EXPECT_TRUE(client.created_options.loop_execution);
    EXPECT_EQ(client.created_options.loop_run_id, observed.id);
    EXPECT_EQ(client.created_options.cwd, root.string());
    EXPECT_TRUE(observed.worktree_path.empty());
    EXPECT_TRUE(observed.worktree_branch.empty());
    EXPECT_EQ(registry.size(), 0u);
}

} // namespace
} // namespace acecode::loop
