#include <gtest/gtest.h>

#include "agent_loop.hpp"
#include "permissions.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "stub_provider.hpp"
#include "tool/file_write_tool.hpp"
#include "tool/plan_mode_tool.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
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

std::string read_file(const fs::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(ifs),
                       std::istreambuf_iterator<char>());
}

class PlanModeWriteHarness {
public:
    explicit PlanModeWriteHarness(std::string cwd)
        : cwd_(std::move(cwd)) {
        project_dir_ = acecode::SessionStorage::get_project_dir(cwd_);
        fs::remove_all(project_dir_);

        perms_.set_mode(acecode::PermissionMode::Plan);
        callbacks_.on_busy_changed = [this](bool busy) {
            std::lock_guard<std::mutex> lk(busy_mu_);
            busy_ = busy;
            if (!busy) busy_cv_.notify_all();
        };
        callbacks_.on_tool_confirm = [this](const std::string&, const std::string&) {
            ++confirm_count_;
            return acecode::PermissionResult::Deny;
        };

        tools_.register_tool(acecode::create_file_write_tool());
        auto accessor = [this]() -> std::shared_ptr<acecode::LlmProvider> { return provider_; };
        loop_ = std::make_unique<acecode::AgentLoop>(accessor, tools_, callbacks_, cwd_, perms_);
        sm_.start_session(cwd_, "stub", "stub-model", "sid-agent-plan-mode");
        loop_->set_session_manager(&sm_);
    }

    ~PlanModeWriteHarness() {
        loop_.reset();
        if (!project_dir_.empty()) fs::remove_all(project_dir_);
        if (!cwd_.empty()) fs::remove_all(cwd_);
    }

    acecode_test::StubLlmProvider& provider() { return *provider_; }
    acecode::SessionManager& session_manager() { return sm_; }
    int confirm_count() const { return confirm_count_; }

    bool submit_and_wait(std::chrono::milliseconds timeout = 5s) {
        {
            std::lock_guard<std::mutex> lk(busy_mu_);
            busy_ = true;
        }
        loop_->submit("go");
        std::unique_lock<std::mutex> lk(busy_mu_);
        return busy_cv_.wait_for(lk, timeout, [this] { return !busy_; });
    }

private:
    std::string cwd_;
    fs::path project_dir_;
    std::shared_ptr<acecode_test::StubLlmProvider> provider_ =
        std::make_shared<acecode_test::StubLlmProvider>();
    acecode::ToolExecutor tools_;
    acecode::PermissionManager perms_;
    acecode::AgentCallbacks callbacks_;
    acecode::SessionManager sm_;
    std::unique_ptr<acecode::AgentLoop> loop_;

    mutable std::mutex busy_mu_;
    std::condition_variable busy_cv_;
    bool busy_ = false;
    int confirm_count_ = 0;
};

class YoloEnterPlanHarness {
public:
    explicit YoloEnterPlanHarness(std::string cwd)
        : cwd_(std::move(cwd)) {
        project_dir_ = acecode::SessionStorage::get_project_dir(cwd_);
        fs::remove_all(project_dir_);

        perms_.set_mode(acecode::PermissionMode::Yolo);
        callbacks_.on_busy_changed = [this](bool busy) {
            std::lock_guard<std::mutex> lk(busy_mu_);
            busy_ = busy;
            if (!busy) busy_cv_.notify_all();
        };
        callbacks_.on_tool_confirm = [this](const std::string&, const std::string&) {
            ++confirm_count_;
            return acecode::PermissionResult::Deny;
        };

        tools_.register_tool(acecode::create_enter_plan_mode_tool());
        auto accessor = [this]() -> std::shared_ptr<acecode::LlmProvider> { return provider_; };
        loop_ = std::make_unique<acecode::AgentLoop>(accessor, tools_, callbacks_, cwd_, perms_);
        sm_.start_session(cwd_, "stub", "stub-model", "sid-agent-yolo-plan-mode");
        sm_.set_permission_mode("yolo");
        loop_->set_session_manager(&sm_);
    }

    ~YoloEnterPlanHarness() {
        loop_.reset();
        if (!project_dir_.empty()) fs::remove_all(project_dir_);
        if (!cwd_.empty()) fs::remove_all(cwd_);
    }

    acecode_test::StubLlmProvider& provider() { return *provider_; }
    acecode::PermissionMode permission_mode() const { return perms_.mode(); }
    std::string session_permission_mode() const { return sm_.current_permission_mode(); }
    std::string session_pre_plan_mode() const { return sm_.current_pre_plan_permission_mode(); }
    int confirm_count() const { return confirm_count_; }

    bool submit_and_wait(std::chrono::milliseconds timeout = 5s) {
        {
            std::lock_guard<std::mutex> lk(busy_mu_);
            busy_ = true;
        }
        loop_->submit("go");
        std::unique_lock<std::mutex> lk(busy_mu_);
        return busy_cv_.wait_for(lk, timeout, [this] { return !busy_; });
    }

private:
    std::string cwd_;
    fs::path project_dir_;
    std::shared_ptr<acecode_test::StubLlmProvider> provider_ =
        std::make_shared<acecode_test::StubLlmProvider>();
    acecode::ToolExecutor tools_;
    acecode::PermissionManager perms_;
    acecode::AgentCallbacks callbacks_;
    acecode::SessionManager sm_;
    std::unique_ptr<acecode::AgentLoop> loop_;

    mutable std::mutex busy_mu_;
    std::condition_variable busy_cv_;
    bool busy_ = false;
    int confirm_count_ = 0;
};

} // namespace

TEST(AgentLoopPlanMode, WritesActivePlanFileWithoutPermissionPrompt) {
    auto cwd = make_temp_dir("acecode_agent_plan_file_allow");
    PlanModeWriteHarness h(cwd.string());
    const std::string plan_path = h.session_manager().ensure_plan_file_path();
    ASSERT_FALSE(plan_path.empty());

    const std::string content = "1. Inspect\n2. Implement\n";
    h.provider().push_tool_call("file_write", nlohmann::json{
        {"file_path", plan_path},
        {"content", content}
    }.dump(), "write-plan");
    h.provider().push_text("done");

    ASSERT_TRUE(h.submit_and_wait());
    EXPECT_EQ(h.confirm_count(), 0);
    EXPECT_EQ(read_file(plan_path), content);
}

TEST(AgentLoopPlanMode, NonPlanFileWriteStillRequiresPermissionPrompt) {
    auto cwd = make_temp_dir("acecode_agent_plan_file_deny_other");
    PlanModeWriteHarness h(cwd.string());
    const auto target = cwd / "implementation.md";

    h.provider().push_tool_call("file_write", nlohmann::json{
        {"file_path", target.string()},
        {"content", "implementation change\n"}
    }.dump(), "write-other");
    h.provider().push_text("done");

    ASSERT_TRUE(h.submit_and_wait());
    EXPECT_EQ(h.confirm_count(), 1);
    EXPECT_FALSE(fs::exists(target));
}

TEST(AgentLoopPlanMode, EnterPlanModeToolDoesNotLeaveYoloMode) {
    auto cwd = make_temp_dir("acecode_agent_yolo_enter_plan_noop");
    YoloEnterPlanHarness h(cwd.string());

    h.provider().push_tool_call("EnterPlanMode", "{}", "enter-plan");
    h.provider().push_text("done");

    ASSERT_TRUE(h.submit_and_wait());
    EXPECT_EQ(h.confirm_count(), 0);
    EXPECT_EQ(h.permission_mode(), acecode::PermissionMode::Yolo);
    EXPECT_EQ(h.session_permission_mode(), "yolo");
    EXPECT_TRUE(h.session_pre_plan_mode().empty());
}
