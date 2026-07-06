#include <gtest/gtest.h>

#include "agent_loop.hpp"
#include "permissions.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "session/thread_goal_store.hpp"
#include "stub_provider.hpp"
#include "tool/goal_tool.hpp"
#include "tool/tool_executor.hpp"

#include <atomic>
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
        // 记录写工具权限确认请求。既有用例只用只读的 goal 工具(Phase 1,
        // 不触发确认),装上这个回调不影响它们;无人值守用例靠它断言
        // 「goal active 时确认回调绝不被触发」。
        cb.on_tool_confirm = [this](const std::string&, const std::string&) {
            confirm_requests_.fetch_add(1);
            return acecode::PermissionResult::Allow;
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
    acecode::ToolExecutor& tools() { return tools_; }
    acecode::PermissionManager& permissions() { return perms_; }
    int confirm_requests() const { return confirm_requests_.load(); }

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

    // 在指定 turn 的请求消息里找包含 needle 的 user 消息(steering 注入断言用)。
    bool turn_request_has_user_message_containing(int turn_index,
                                                  const std::string& needle) {
        for (const auto& msg : provider_->messages_for_turn(turn_index)) {
            if (msg.role == "user" &&
                msg.content.find(needle) != std::string::npos) {
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
    std::atomic<int> confirm_requests_{0};
};

// 造一个需要权限确认的写工具(is_read_only=false):default 权限模式下
// should_auto_allow 为 false,会走确认门。executed 记录工具是否真的跑了。
acecode::ToolImpl make_fake_write_tool(std::atomic<int>& executed) {
    acecode::ToolImpl impl;
    impl.definition.name = "fake_write";
    impl.definition.description = "test-only write tool";
    impl.definition.parameters = {
        {"type", "object"},
        {"properties", nlohmann::json::object()},
    };
    impl.is_read_only = false;
    impl.execute = [&executed](const std::string&,
                               const acecode::ToolContext&) -> acecode::ToolResult {
        executed.fetch_add(1);
        return acecode::ToolResult{"ok", true};
    };
    return impl;
}

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

TEST(AgentLoopGoal, ResumeAfterAbortClearsStaleAbortAndContinues) {
    AgentLoopGoalHarness h("abort_resume");
    h.create_goal();
    h.provider().set_latency_ms(200);

    std::thread aborter([&h] {
        std::this_thread::sleep_for(50ms);
        h.loop().abort();
    });
    ASSERT_TRUE(h.submit_and_wait("start", 10s));
    aborter.join();

    auto paused = h.goal();
    ASSERT_TRUE(paused.has_value());
    ASSERT_EQ(paused->status, acecode::ThreadGoalStatus::Paused);

    h.provider().set_latency_ms(0);
    h.provider().push_tool_call("update_goal", R"({"status":"complete"})", "goal-resumed-done");
    ASSERT_TRUE(h.session_manager().goal_store()->update_thread_goal_status(
        h.session_manager().current_session_id(),
        paused->goal_id,
        acecode::ThreadGoalStatus::Active));

    h.loop().clear_stale_abort_request();
    h.loop().maybe_continue_goal();

    ASSERT_TRUE(h.wait_until([&h] {
        auto goal = h.goal();
        return goal.has_value() &&
            goal->status == acecode::ThreadGoalStatus::Complete &&
            h.saw_goal_status(acecode::ThreadGoalStatus::Complete);
    }, 10s));
    EXPECT_TRUE(h.has_hidden_goal_context_message());
    EXPECT_GE(h.provider().turn_count(), 2);
}

TEST(AgentLoopGoal, IdleContinuationUsesHiddenContextAndStopsWhenComplete) {
    AgentLoopGoalHarness h("continue");
    h.create_goal();
    h.provider().push_text("progress");
    h.provider().push_tool_call("update_goal", R"({"status":"complete"})", "goal-done");

    ASSERT_TRUE(h.submit_and_wait("start"));
    ASSERT_TRUE(h.wait_until([&h] {
        auto goal = h.goal();
        return goal.has_value() &&
            goal->status == acecode::ThreadGoalStatus::Complete &&
            h.saw_goal_status(acecode::ThreadGoalStatus::Complete);
    }, 10s));

    EXPECT_TRUE(h.has_hidden_goal_context_message());
    EXPECT_TRUE(h.saw_goal_status(acecode::ThreadGoalStatus::Complete));
    EXPECT_GE(h.provider().turn_count(), 2);
    auto statuses = h.goal_statuses();
    ASSERT_FALSE(statuses.empty());
    EXPECT_NE(statuses.back().find("complete"), std::string::npos);
}

// 场景:goal Active 时 provider 首轮直接报终止错误(HTTP 500,重试已在
// provider 层耗尽)。期望:goal 被标记 blocked,自动 continuation 不再触发
// (provider 只被调用 1 次)。
// 回归:修复前回合出错后 maybe_continue_goal 照常触发,goal 保持 Active,
// 对着同一个错误无限重试烧 token(对齐 Codex ext/goal 的 on_turn_error)。
TEST(AgentLoopGoal, ProviderErrorBlocksGoalAndStopsContinuation) {
    AgentLoopGoalHarness h("provider_error");
    h.create_goal();

    acecode::ProviderErrorInfo info;
    info.kind = acecode::ProviderErrorKind::Http;
    info.status_code = 500;
    info.display_message = "internal server error";
    h.provider().push_error(info);

    ASSERT_TRUE(h.submit_and_wait("start"));
    ASSERT_TRUE(h.wait_until([&h] {
        auto goal = h.goal();
        return goal.has_value() &&
            goal->status == acecode::ThreadGoalStatus::Blocked;
    }));
    EXPECT_TRUE(h.saw_goal_status(acecode::ThreadGoalStatus::Blocked));

    // 给 continuation 一个触发窗口再断言:provider 只被调用了 1 次。
    // 300ms 远大于 maybe_continue_goal 入队 + worker 取任务的耗时。
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(h.provider().turn_count(), 1);
}

// 场景:provider 报 HTTP 429(限流,重试已耗尽)。期望:goal 标记
// usage_limited 而不是 blocked —— 语义与 Codex 一致,限流是「等额度恢复后
// /goal resume 可继续」,与真正的阻塞区分开。
TEST(AgentLoopGoal, RateLimit429MarksGoalUsageLimited) {
    AgentLoopGoalHarness h("rate_limit");
    h.create_goal();

    acecode::ProviderErrorInfo info;
    info.kind = acecode::ProviderErrorKind::Http;
    info.status_code = 429;
    info.display_message = "rate limited";
    h.provider().push_error(info);

    ASSERT_TRUE(h.submit_and_wait("start"));
    ASSERT_TRUE(h.wait_until([&h] {
        auto goal = h.goal();
        return goal.has_value() &&
            goal->status == acecode::ThreadGoalStatus::UsageLimited;
    }));
    EXPECT_TRUE(h.saw_goal_status(acecode::ThreadGoalStatus::UsageLimited));
}

// 场景:default 权限模式 + Active goal,模型调用需要确认的写工具。期望:
// 确认回调一次都不触发,工具直接执行(goal 无人值守自动放行)。
// 回归:修复前 goal continuation 回合里写工具会弹出权限确认窗,goal
// 「无人值守持续推进」的核心承诺被打断。
TEST(AgentLoopGoal, UnattendedGoalAutoApprovesWriteToolWithoutConfirm) {
    AgentLoopGoalHarness h("auto_approve");
    std::atomic<int> executed{0};
    h.tools().register_tool(make_fake_write_tool(executed));
    h.create_goal();

    h.provider().push_tool_call("fake_write", "{}", "call-write");
    h.provider().push_tool_call("update_goal", R"({"status":"complete"})", "goal-done");

    ASSERT_TRUE(h.submit_and_wait("start", 10s));
    ASSERT_TRUE(h.wait_until([&h] {
        auto goal = h.goal();
        return goal.has_value() &&
            goal->status == acecode::ThreadGoalStatus::Complete;
    }, 10s));
    EXPECT_EQ(h.confirm_requests(), 0);
    EXPECT_EQ(executed.load(), 1);
}

// 对照场景:没有 goal 时行为不变 —— 同一个写工具仍走确认回调(返回 Allow
// 后执行)。保证自动放行只在 goal active 时生效,不弱化平时的权限模型。
TEST(AgentLoopGoal, WriteToolStillConfirmsWithoutActiveGoal) {
    AgentLoopGoalHarness h("confirm_baseline");
    std::atomic<int> executed{0};
    h.tools().register_tool(make_fake_write_tool(executed));

    h.provider().push_tool_call("fake_write", "{}", "call-write");
    h.provider().push_text("done");

    ASSERT_TRUE(h.submit_and_wait("start", 10s));
    EXPECT_EQ(h.confirm_requests(), 1);
    EXPECT_EQ(executed.load(), 1);
}

// 场景:goal 预算极小(1 token),首轮 provider 调用后估算入账即翻
// budget_limited,但回合还有后续迭代。期望:下一次模型请求包含
// budget_limit wrap-up 提示(总结进展、不再开新活)。
// 回归:修复前只给用户发了一条 system 消息,模型完全不知道预算已耗尽,
// 会在当前回合继续开新工作(对齐 Codex budget_limit steering)。
TEST(AgentLoopGoal, BudgetLimitSteeringReachesRunningTurn) {
    AgentLoopGoalHarness h("budget_steering");
    h.create_goal(1);

    h.provider().push_tool_call("get_goal", "{}", "call-get");
    h.provider().push_text("wrapping up");

    ASSERT_TRUE(h.submit_and_wait("start", 10s));
    ASSERT_GE(h.provider().turn_count(), 2);
    EXPECT_TRUE(h.turn_request_has_user_message_containing(
        1, "reached its token budget"));
}

// 场景:回合运行中用户 /goal edit 修改 objective。期望:同一回合的下一次
// 模型请求包含 objective_updated 提示与新 objective 文本,模型立即转向。
// 回归:修复前运行中的回合对 objective 变更毫无感知,直到下一次
// continuation 才看到新目标(对齐 Codex objective_updated steering)。
TEST(AgentLoopGoal, ObjectiveEditSteeringReachesRunningTurn) {
    AgentLoopGoalHarness h("objective_steering");
    h.create_goal();
    // 200ms 延迟给「回合运行中编辑 objective」留出确定性窗口。
    h.provider().set_latency_ms(200);
    h.provider().push_tool_call("get_goal", "{}", "call-get");
    h.provider().push_tool_call("update_goal", R"({"status":"complete"})", "goal-done");

    // gtest 的 ASSERT_* 不能在非主线程用(fatal failure 只中止该线程),
    // 编辑线程里只做动作;编辑没生效时靠主线程的内容断言兜底报错。
    std::thread editor([&h] {
        std::this_thread::sleep_for(50ms);
        auto goal = h.goal();
        if (!goal.has_value()) return;
        h.session_manager().goal_store()->update_thread_goal_objective(
            h.session_manager().current_session_id(), goal->goal_id,
            "pivot to the new objective", goal->token_budget);
        h.loop().notify_goal_objective_updated();
    });
    ASSERT_TRUE(h.submit_and_wait("start", 15s));
    editor.join();

    ASSERT_GE(h.provider().turn_count(), 2);
    EXPECT_TRUE(h.turn_request_has_user_message_containing(
        1, "objective was edited"));
    EXPECT_TRUE(h.turn_request_has_user_message_containing(
        1, "pivot to the new objective"));
}

// 场景:Plan mode 下 goal 处于 Active。期望:maybe_continue_goal 不自动开
// 新回合(provider 不被调用)。Plan mode 的只读约束优先于 goal 自动续跑,
// 对齐 Codex try_start_turn_if_idle 的 PlanMode 拒绝。
TEST(AgentLoopGoal, PlanModeSuppressesGoalContinuation) {
    AgentLoopGoalHarness h("plan_mode");
    h.create_goal();
    h.permissions().set_mode(acecode::PermissionMode::Plan);

    h.loop().maybe_continue_goal();
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(h.provider().turn_count(), 0);

    // 退出 plan mode 后 continuation 恢复正常。
    h.permissions().set_mode(acecode::PermissionMode::Default);
    h.provider().push_tool_call("update_goal", R"({"status":"complete"})", "goal-done");
    h.loop().maybe_continue_goal();
    ASSERT_TRUE(h.wait_until([&h] {
        auto goal = h.goal();
        return goal.has_value() &&
            goal->status == acecode::ThreadGoalStatus::Complete;
    }, 10s));
}

// 场景:goal continuation 的隐藏上下文提示。期望:包含新补齐的段落 ——
// Unattended mode(无人值守说明)/ Work from evidence / Fidelity /
// completion audit 的逐条要求。这是对 Codex continuation.md 模板的对齐,
// 防止后续改 prompt 时静默丢段。
TEST(AgentLoopGoal, ContinuationPromptCoversAuditAndUnattendedSections) {
    AgentLoopGoalHarness h("prompt_content");
    h.create_goal();
    h.provider().push_text("progress");
    h.provider().push_tool_call("update_goal", R"({"status":"complete"})", "goal-done");

    ASSERT_TRUE(h.submit_and_wait("start"));
    ASSERT_TRUE(h.wait_until([&h] {
        auto goal = h.goal();
        return goal.has_value() &&
            goal->status == acecode::ThreadGoalStatus::Complete;
    }, 10s));

    std::string continuation;
    for (const auto& msg : h.loop().messages()) {
        if (msg.role == "user" && msg.metadata.is_object() &&
            msg.metadata.value("hidden_goal_context", false) &&
            msg.content.find("<goal_context>") != std::string::npos) {
            continuation = msg.content;
            break;
        }
    }
    ASSERT_FALSE(continuation.empty());
    EXPECT_NE(continuation.find("Unattended mode:"), std::string::npos);
    EXPECT_NE(continuation.find("Work from evidence:"), std::string::npos);
    EXPECT_NE(continuation.find("Fidelity:"), std::string::npos);
    EXPECT_NE(continuation.find("Do not call AskUserQuestion"), std::string::npos);
    EXPECT_NE(continuation.find("treat completion as unproven"), std::string::npos);
}
