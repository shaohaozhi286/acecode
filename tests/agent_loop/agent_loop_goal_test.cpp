#include <gtest/gtest.h>

#include "agent_loop.hpp"
#include "permissions.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "session/thread_goal_store.hpp"
#include "stub_provider.hpp"
#include "tool/goal_tool.hpp"
#include "tool/tool_executor.hpp"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace fs = std::filesystem;

namespace {

fs::path temp_cwd(const std::string& hint) {
    auto dir = fs::temp_directory_path() /
        ("acecode_agent_goal_" + hint + "_" + std::to_string(std::random_device{}()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    fs::remove_all(acecode::SessionStorage::get_project_dir(dir.string()));
    return dir;
}

class AgentLoopGoalHarness {
public:
    explicit AgentLoopGoalHarness(const std::string& hint)
        : cwd_(temp_cwd(hint)) {
        sm_.start_session(cwd_.string(), "stub", "stub-1", "sid-" + hint);
        tools_.register_tool(acecode::create_get_goal_tool());
        tools_.register_tool(acecode::create_create_goal_tool());
        tools_.register_tool(acecode::create_update_goal_tool());

        acecode::AgentCallbacks cb;
        cb.on_busy_changed = [this](bool busy) {
            std::lock_guard<std::mutex> lk(busy_mu_);
            busy_ = busy;
            if (!busy) busy_cv_.notify_all();
        };
        cb.on_goal_status = [this](const std::string& status) {
            std::lock_guard<std::mutex> lk(goal_status_mu_);
            goal_statuses_.push_back(status);
        };

        auto accessor = [this]() -> std::shared_ptr<acecode::LlmProvider> {
            return provider_;
        };
        loop_ = std::make_unique<acecode::AgentLoop>(
            accessor, tools_, cb, cwd_.string(), perms_);
        loop_->set_session_manager(&sm_);
        sub_ = loop_->events().subscribe([this](const acecode::SessionEvent& evt) {
            std::lock_guard<std::mutex> lk(events_mu_);
            events_.push_back(evt);
        });
    }

    ~AgentLoopGoalHarness() {
        if (loop_ && sub_ != 0) loop_->events().unsubscribe(sub_);
        loop_.reset();
        fs::remove_all(cwd_);
        fs::remove_all(acecode::SessionStorage::get_project_dir(cwd_.string()));
    }

    acecode_test::StubLlmProvider& provider() { return *provider_; }
    acecode::SessionManager& session_manager() { return sm_; }
    acecode::AgentLoop& loop() { return *loop_; }

    void create_goal(std::optional<std::int64_t> budget = std::nullopt) {
        const std::string sid = sm_.ensure_active_session_id();
        ASSERT_FALSE(sid.empty());
        ASSERT_TRUE(sm_.goal_store()->replace_thread_goal(
            sid, "finish the goal", budget, acecode::ThreadGoalStatus::Active));
        loop_->restore_goal_runtime();
    }

    std::optional<acecode::ThreadGoal> goal() {
        return sm_.goal_store()->get_thread_goal(sm_.current_session_id());
    }

    bool submit_and_wait(const std::string& prompt,
                         std::chrono::milliseconds timeout = 5s) {
        {
            std::lock_guard<std::mutex> lk(busy_mu_);
            busy_ = true;
        }
        loop_->submit(prompt);
        std::unique_lock<std::mutex> lk(busy_mu_);
        return busy_cv_.wait_for(lk, timeout, [this] { return !busy_; });
    }

    template <typename Predicate>
    bool wait_until(Predicate pred, std::chrono::milliseconds timeout = 5s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (pred()) return true;
            std::this_thread::sleep_for(10ms);
        }
        return pred();
    }

    bool saw_goal_status(acecode::ThreadGoalStatus status) const {
        std::lock_guard<std::mutex> lk(events_mu_);
        for (const auto& evt : events_) {
            if (evt.kind != acecode::SessionEventKind::GoalUpdated) continue;
            if (evt.payload["goal"].value("status", "") == acecode::to_string(status)) {
                return true;
            }
        }
        return false;
    }

    bool has_hidden_goal_context_message() const {
        for (const auto& msg : loop_->messages()) {
            if (msg.metadata.is_object() &&
                msg.metadata.value("hidden_goal_context", false)) {
                return true;
            }
        }
        return false;
    }

    std::vector<std::string> goal_statuses() const {
        std::lock_guard<std::mutex> lk(goal_status_mu_);
        return goal_statuses_;
    }

private:
    fs::path cwd_;
    std::shared_ptr<acecode_test::StubLlmProvider> provider_ =
        std::make_shared<acecode_test::StubLlmProvider>();
    acecode::ToolExecutor tools_;
    acecode::PermissionManager perms_;
    acecode::SessionManager sm_;
    std::unique_ptr<acecode::AgentLoop> loop_;
    acecode::EventDispatcher::SubscriptionId sub_ = 0;

    mutable std::mutex busy_mu_;
    std::condition_variable busy_cv_;
    bool busy_ = false;

    mutable std::mutex events_mu_;
    std::vector<acecode::SessionEvent> events_;

    mutable std::mutex goal_status_mu_;
    std::vector<std::string> goal_statuses_;
};

} // namespace

TEST(AgentLoopGoal, EstimatedUsageCanBudgetLimitWithoutProviderUsage) {
    AgentLoopGoalHarness h("budget");
    h.create_goal(1);
    h.provider().push_text("done");

    ASSERT_TRUE(h.submit_and_wait("start"));
    auto goal = h.goal();
    ASSERT_TRUE(goal.has_value());
    EXPECT_EQ(goal->status, acecode::ThreadGoalStatus::BudgetLimited);
    EXPECT_GT(goal->tokens_used, 0);
    EXPECT_TRUE(h.saw_goal_status(acecode::ThreadGoalStatus::BudgetLimited));
}

TEST(AgentLoopGoal, AbortPausesActiveGoalAfterAccounting) {
    AgentLoopGoalHarness h("abort");
    h.create_goal();
    h.provider().set_latency_ms(200);

    std::thread aborter([&h] {
        std::this_thread::sleep_for(50ms);
        h.loop().abort();
    });
    ASSERT_TRUE(h.submit_and_wait("start", 10s));
    aborter.join();

    auto goal = h.goal();
    ASSERT_TRUE(goal.has_value());
    EXPECT_EQ(goal->status, acecode::ThreadGoalStatus::Paused);
    EXPECT_TRUE(h.saw_goal_status(acecode::ThreadGoalStatus::Paused));
}

TEST(AgentLoopGoal, IdleContinuationUsesHiddenContextAndStopsWhenComplete) {
    AgentLoopGoalHarness h("continue");
    h.create_goal();
    h.provider().push_text("progress");
    h.provider().push_tool_call("update_goal", R"({"status":"complete"})", "goal-done");

    ASSERT_TRUE(h.submit_and_wait("start"));
    ASSERT_TRUE(h.wait_until([&h] {
        auto goal = h.goal();
        return goal.has_value() && goal->status == acecode::ThreadGoalStatus::Complete;
    }, 10s));

    EXPECT_TRUE(h.has_hidden_goal_context_message());
    EXPECT_TRUE(h.saw_goal_status(acecode::ThreadGoalStatus::Complete));
    EXPECT_GE(h.provider().turn_count(), 2);
    auto statuses = h.goal_statuses();
    ASSERT_FALSE(statuses.empty());
    EXPECT_NE(statuses.back().find("complete"), std::string::npos);
}
