#include <gtest/gtest.h>

#include "agent_loop.hpp"
#include "permissions.hpp"
#include "session/session_manager.hpp"
#include "stub_provider.hpp"
#include "tool/todo_write_tool.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <vector>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

fs::path make_temp_dir(const std::string& name) {
    auto p = fs::temp_directory_path() /
        (name + "_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
         "_" + std::to_string(reinterpret_cast<std::uintptr_t>(&name)));
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

class TodoWriteHarness {
public:
    explicit TodoWriteHarness(std::string cwd)
        : cwd_(std::move(cwd)) {
        callbacks_.on_busy_changed = [this](bool busy) {
            std::lock_guard<std::mutex> lk(busy_mu_);
            busy_ = busy;
            if (!busy) busy_cv_.notify_all();
        };
        callbacks_.on_tool_confirm = [this](const std::string&, const std::string&) {
            ++confirm_count_;
            return acecode::PermissionResult::Deny;
        };
        callbacks_.on_todo_updated = [this](const nlohmann::json& payload) {
            std::lock_guard<std::mutex> lk(todo_mu_);
            todo_callbacks_.push_back(payload);
        };
        tools_.register_tool(acecode::create_todo_write_tool());
        auto accessor = [this]() -> std::shared_ptr<acecode::LlmProvider> { return provider_; };
        loop_ = std::make_unique<acecode::AgentLoop>(accessor, tools_, callbacks_, cwd_, perms_);
        sm_.start_session(cwd_, "stub", "stub-model", "20260603-130000-abcd");
        loop_->set_session_manager(&sm_);
        sub_ = loop_->events().subscribe([this](const acecode::SessionEvent& e) {
            std::lock_guard<std::mutex> lk(events_mu_);
            events_.push_back(e);
        });
    }

    ~TodoWriteHarness() {
        if (loop_ && sub_ != 0) loop_->events().unsubscribe(sub_);
        loop_.reset();
    }

    acecode_test::StubLlmProvider& provider() { return *provider_; }
    int confirm_count() const { return confirm_count_; }
    acecode::SessionManager& session_manager() { return sm_; }

    bool submit_and_wait(std::chrono::milliseconds timeout = 5s) {
        {
            std::lock_guard<std::mutex> lk(busy_mu_);
            busy_ = true;
        }
        loop_->submit("go");
        std::unique_lock<std::mutex> lk(busy_mu_);
        return busy_cv_.wait_for(lk, timeout, [this] { return !busy_; });
    }

    std::vector<acecode::SessionEvent> events_of(acecode::SessionEventKind kind) const {
        std::lock_guard<std::mutex> lk(events_mu_);
        std::vector<acecode::SessionEvent> out;
        for (const auto& event : events_) {
            if (event.kind == kind) out.push_back(event);
        }
        return out;
    }

    std::vector<nlohmann::json> todo_callbacks() const {
        std::lock_guard<std::mutex> lk(todo_mu_);
        return todo_callbacks_;
    }

private:
    std::string cwd_;
    std::shared_ptr<acecode_test::StubLlmProvider> provider_ =
        std::make_shared<acecode_test::StubLlmProvider>();
    acecode::ToolExecutor tools_;
    acecode::PermissionManager perms_;
    acecode::AgentCallbacks callbacks_;
    acecode::SessionManager sm_;
    std::unique_ptr<acecode::AgentLoop> loop_;
    acecode::EventDispatcher::SubscriptionId sub_ = 0;

    mutable std::mutex events_mu_;
    std::vector<acecode::SessionEvent> events_;
    mutable std::mutex todo_mu_;
    std::vector<nlohmann::json> todo_callbacks_;
    mutable std::mutex busy_mu_;
    std::condition_variable busy_cv_;
    bool busy_ = false;
    int confirm_count_ = 0;
};

} // namespace

TEST(AgentLoopTodoWrite, EmitsTodoUpdatedWithoutPermissionPrompt) {
    auto cwd = make_temp_dir("acecode_agent_todowrite");
    TodoWriteHarness h(cwd.string());

    nlohmann::json args = {
        {"todos", nlohmann::json::array({
            {{"id", "1"}, {"content", "Inspect Hermes"}, {"status", "completed"}},
            {{"id", "2"}, {"content", "Wire checklist UI"}, {"status", "in_progress"}},
        })},
    };
    h.provider().push_tool_call("TodoWrite", args.dump(), "todo-1");
    h.provider().push_text("done");

    ASSERT_TRUE(h.submit_and_wait());
    EXPECT_EQ(h.confirm_count(), 0);

    auto todo_events = h.events_of(acecode::SessionEventKind::TodoUpdated);
    ASSERT_EQ(todo_events.size(), 1u);
    EXPECT_EQ(todo_events[0].payload["summary"]["total"], 2);
    EXPECT_EQ(todo_events[0].payload["summary"]["completed"], 1);
    EXPECT_EQ(todo_events[0].payload["todos"][1]["status"], "in_progress");

    auto callbacks = h.todo_callbacks();
    ASSERT_EQ(callbacks.size(), 1u);
    EXPECT_EQ(callbacks[0]["todos"][0]["content"], "Inspect Hermes");

    auto todos = h.session_manager().current_todos();
    ASSERT_EQ(todos.size(), 2u);
    EXPECT_EQ(todos[0].status, "completed");

    fs::remove_all(cwd);
}
