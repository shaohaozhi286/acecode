// 覆盖 AgentLoop daemon 事件流里的工具生命周期事件。
// 重点锁住 improve-web-agent-progress-feedback:read-only 并行工具、同名工具、
// 验证失败、权限拒绝都必须发 keyed tool_start/tool_end,避免 Web 上工具卡片卡住。

#include <gtest/gtest.h>

#include "agent_loop.hpp"
#include "permissions.hpp"
#include "provider/llm_provider.hpp"
#include "session/event_dispatcher.hpp"
#include "stub_provider.hpp"
#include "tool/file_edit_tool.hpp"
#include "tool/mtime_tracker.hpp"
#include "tool/tool_executor.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using acecode::AgentCallbacks;
using acecode::AgentLoop;
using acecode::PermissionManager;
using acecode::PermissionResult;
using acecode::SessionEvent;
using acecode::SessionEventKind;
using acecode::ToolContext;
using acecode::ToolDef;
using acecode::ToolExecutor;
using acecode::ToolImpl;
using acecode::ToolResult;
using acecode::ToolSource;
using acecode_test::ScriptedResponse;
using acecode_test::StubLlmProvider;

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

ToolImpl make_probe_tool(std::string name, bool read_only, std::atomic<int>* calls = nullptr) {
    ToolDef def;
    def.name = name;
    def.description = "Lifecycle probe tool";
    def.parameters = {
        {"type", "object"},
        {"properties", nlohmann::json::object({
            {"file_path", {{"type", "string"}}},
        })},
    };

    ToolImpl impl;
    impl.definition = def;
    impl.is_read_only = read_only;
    impl.source = ToolSource::Builtin;
    impl.execute = [calls](const std::string&, const ToolContext&) -> ToolResult {
        if (calls) calls->fetch_add(1);
        return ToolResult{"probe ok", true};
    };
    return impl;
}

ToolImpl make_fake_bash_tool(std::atomic<int>* calls) {
    ToolDef def;
    def.name = "bash";
    def.description = "Fake bash for AgentLoop guard tests";
    def.parameters = {
        {"type", "object"},
        {"properties", nlohmann::json::object({
            {"command", {{"type", "string"}}},
        })},
    };
    ToolImpl impl;
    impl.definition = def;
    impl.is_read_only = false;
    impl.source = ToolSource::Builtin;
    impl.execute = [calls](const std::string&, const ToolContext&) -> ToolResult {
        if (calls) calls->fetch_add(1);
        return ToolResult{"fake bash executed", true};
    };
    return impl;
}

fs::path make_temp_dir(const std::string& name) {
    auto p = fs::temp_directory_path() /
        (name + "_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
         "_" + std::to_string(reinterpret_cast<std::uintptr_t>(&name)));
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

class ToolLifecycleHarness {
public:
    explicit ToolLifecycleHarness(std::string cwd)
        : cwd_(std::move(cwd)) {
        AgentCallbacks cb;
        cb.on_busy_changed = [this](bool busy) {
            std::lock_guard<std::mutex> lk(busy_mu_);
            busy_ = busy;
            if (!busy) busy_cv_.notify_all();
        };
        cb.on_tool_confirm = [this](const std::string&, const std::string&) {
            confirm_count_.fetch_add(1);
            return PermissionResult::Deny;
        };
        auto accessor = [this]() -> std::shared_ptr<acecode::LlmProvider> { return provider_; };
        loop_ = std::make_unique<AgentLoop>(accessor, tools_, cb, cwd_, perms_);
        sub_ = loop_->events().subscribe([this](const SessionEvent& e) {
            std::lock_guard<std::mutex> lk(events_mu_);
            events_.push_back(e);
        });
    }

    ~ToolLifecycleHarness() {
        if (loop_ && sub_ != 0) loop_->events().unsubscribe(sub_);
        loop_.reset();
    }

    ToolExecutor& tools() { return tools_; }
    StubLlmProvider& provider() { return *provider_; }
    std::atomic<int>& confirm_count() { return confirm_count_; }

    bool submit_and_wait(std::chrono::milliseconds timeout = 5s) {
        {
            std::lock_guard<std::mutex> lk(busy_mu_);
            busy_ = true;
        }
        loop_->submit("go");
        std::unique_lock<std::mutex> lk(busy_mu_);
        return busy_cv_.wait_for(lk, timeout, [this] { return !busy_; });
    }

    std::vector<SessionEvent> events_of(SessionEventKind kind) const {
        std::lock_guard<std::mutex> lk(events_mu_);
        std::vector<SessionEvent> out;
        for (const auto& e : events_) {
            if (e.kind == kind) out.push_back(e);
        }
        return out;
    }

private:
    std::string cwd_;
    std::shared_ptr<StubLlmProvider> provider_ = std::make_shared<StubLlmProvider>();
    ToolExecutor tools_;
    PermissionManager perms_;
    std::unique_ptr<AgentLoop> loop_;
    acecode::EventDispatcher::SubscriptionId sub_ = 0;

    mutable std::mutex events_mu_;
    std::vector<SessionEvent> events_;

    std::mutex busy_mu_;
    std::condition_variable busy_cv_;
    bool busy_ = false;
    std::atomic<int> confirm_count_{0};
};

class AllowingToolHarness {
public:
    explicit AllowingToolHarness(std::string cwd)
        : cwd_(std::move(cwd)) {
        AgentCallbacks cb;
        cb.on_busy_changed = [this](bool busy) {
            std::lock_guard<std::mutex> lk(busy_mu_);
            busy_ = busy;
            if (!busy) busy_cv_.notify_all();
        };
        cb.on_tool_confirm = [](const std::string&, const std::string&) {
            return PermissionResult::Allow;
        };
        cb.on_tool_result = [this](const acecode::ChatMessage&,
                                   const std::string& name,
                                   const ToolResult& result) {
            std::lock_guard<std::mutex> lk(results_mu_);
            results_.push_back({name, result});
        };
        auto accessor = [this]() -> std::shared_ptr<acecode::LlmProvider> { return provider_; };
        loop_ = std::make_unique<AgentLoop>(accessor, tools_, cb, cwd_, perms_);
    }

    ~AllowingToolHarness() {
        loop_.reset();
    }

    ToolExecutor& tools() { return tools_; }
    StubLlmProvider& provider() { return *provider_; }

    bool submit_and_wait(std::chrono::milliseconds timeout = 5s) {
        {
            std::lock_guard<std::mutex> lk(busy_mu_);
            busy_ = true;
        }
        loop_->submit("go");
        std::unique_lock<std::mutex> lk(busy_mu_);
        return busy_cv_.wait_for(lk, timeout, [this] { return !busy_; });
    }

    std::vector<std::pair<std::string, ToolResult>> results() const {
        std::lock_guard<std::mutex> lk(results_mu_);
        return results_;
    }

private:
    std::string cwd_;
    std::shared_ptr<StubLlmProvider> provider_ = std::make_shared<StubLlmProvider>();
    ToolExecutor tools_;
    PermissionManager perms_;
    std::unique_ptr<AgentLoop> loop_;

    mutable std::mutex results_mu_;
    std::vector<std::pair<std::string, ToolResult>> results_;

    std::mutex busy_mu_;
    std::condition_variable busy_cv_;
    bool busy_ = false;
};

} // namespace

TEST(AgentLoopToolLifecycleEvents, ReadOnlySameNameParallelToolsEmitKeyedStartAndEnd) {
    auto cwd = make_temp_dir("acecode_lifecycle_ro");
    std::atomic<int> calls{0};
    ToolLifecycleHarness h(cwd.string());
    h.tools().register_tool(make_probe_tool("ro_probe", true, &calls));

    ScriptedResponse tools_turn;
    tools_turn.tool_calls.push_back({"call-a", "ro_probe", "{}"});
    tools_turn.tool_calls.push_back({"call-b", "ro_probe", "{}"});
    h.provider().push_response(std::move(tools_turn));
    h.provider().push_text("done");

    ASSERT_TRUE(h.submit_and_wait());
    EXPECT_EQ(calls.load(), 2);

    auto starts = h.events_of(SessionEventKind::ToolStart);
    auto ends = h.events_of(SessionEventKind::ToolEnd);
    ASSERT_EQ(starts.size(), 2u);
    ASSERT_EQ(ends.size(), 2u);
    EXPECT_EQ(starts[0].payload["tool"], "ro_probe");
    EXPECT_TRUE((starts[0].payload["tool_call_id"] == "call-a" && starts[1].payload["tool_call_id"] == "call-b") ||
                (starts[0].payload["tool_call_id"] == "call-b" && starts[1].payload["tool_call_id"] == "call-a"));
    EXPECT_TRUE(ends[0].payload.value("success", false));
    EXPECT_TRUE(ends[1].payload.value("success", false));
    EXPECT_TRUE((ends[0].payload["tool_call_id"] == "call-a" && ends[1].payload["tool_call_id"] == "call-b") ||
                (ends[0].payload["tool_call_id"] == "call-b" && ends[1].payload["tool_call_id"] == "call-a"));

    fs::remove_all(cwd);
}

TEST(AgentLoopToolLifecycleEvents, ValidationFailureStillEmitsTerminalToolEnd) {
    auto cwd = make_temp_dir("acecode_lifecycle_validation");
    std::atomic<int> calls{0};
    ToolLifecycleHarness h(cwd.string());
    h.tools().register_tool(make_probe_tool("ro_path_probe", true, &calls));

    const auto outside = (cwd.parent_path() / "outside_lifecycle_probe.txt").string();
    ScriptedResponse tools_turn;
    tools_turn.tool_calls.push_back({"call-bad-path", "ro_path_probe",
        nlohmann::json{{"file_path", outside}}.dump()});
    h.provider().push_response(std::move(tools_turn));
    h.provider().push_text("done");

    ASSERT_TRUE(h.submit_and_wait());
    EXPECT_EQ(calls.load(), 0);

    auto starts = h.events_of(SessionEventKind::ToolStart);
    auto ends = h.events_of(SessionEventKind::ToolEnd);
    ASSERT_EQ(starts.size(), 1u);
    ASSERT_EQ(ends.size(), 1u);
    EXPECT_EQ(starts[0].payload["tool_call_id"], "call-bad-path");
    EXPECT_EQ(ends[0].payload["tool_call_id"], "call-bad-path");
    EXPECT_FALSE(ends[0].payload.value("success", true));
    ASSERT_TRUE(ends[0].payload.contains("output"));
    EXPECT_NE(ends[0].payload["output"].get<std::string>().find("Path outside working directory"),
              std::string::npos);

    fs::remove_all(cwd);
}

TEST(AgentLoopToolLifecycleEvents, PermissionDenialStillEmitsTerminalToolEnd) {
    auto cwd = make_temp_dir("acecode_lifecycle_permission");
    std::atomic<int> calls{0};
    ToolLifecycleHarness h(cwd.string());
    h.tools().register_tool(make_probe_tool("write_probe", false, &calls));

    ScriptedResponse tools_turn;
    tools_turn.tool_calls.push_back({"call-deny", "write_probe", "{}"});
    h.provider().push_response(std::move(tools_turn));
    h.provider().push_text("done");

    ASSERT_TRUE(h.submit_and_wait());
    EXPECT_EQ(calls.load(), 0);
    EXPECT_EQ(h.confirm_count().load(), 1);

    auto starts = h.events_of(SessionEventKind::ToolStart);
    auto ends = h.events_of(SessionEventKind::ToolEnd);
    ASSERT_EQ(starts.size(), 1u);
    ASSERT_EQ(ends.size(), 1u);
    EXPECT_EQ(starts[0].payload["tool_call_id"], "call-deny");
    EXPECT_EQ(ends[0].payload["tool_call_id"], "call-deny");
    EXPECT_FALSE(ends[0].payload.value("success", true));
    ASSERT_TRUE(ends[0].payload.contains("output"));
    EXPECT_NE(ends[0].payload["output"].get<std::string>().find("User denied"),
              std::string::npos);

    fs::remove_all(cwd);
}

TEST(AgentLoopToolLifecycleEvents, ShellWriteAfterSafeEditFailureIsBlocked) {
    auto cwd = make_temp_dir("acecode_shell_guard");
    auto target = cwd / "target.txt";
    {
        std::ofstream ofs(target, std::ios::binary);
        ofs << "alpha\n";
    }
    acecode::MtimeTracker::instance().record_read(target.string(), "alpha\n", false);

    std::atomic<int> bash_calls{0};
    AllowingToolHarness h(cwd.string());
    h.tools().register_tool(acecode::create_file_edit_tool());
    h.tools().register_tool(make_fake_bash_tool(&bash_calls));

    h.provider().push_tool_call("file_edit", nlohmann::json{
        {"file_path", target.string()},
        {"old_string", "missing"},
        {"new_string", "beta"}
    }.dump(), "edit-fail");
    h.provider().push_tool_call("bash", nlohmann::json{
        {"command", "type nul > \"" + target.string() + "\""}
    }.dump(), "bash-write");
    h.provider().push_text("done");

    ASSERT_TRUE(h.submit_and_wait());
    EXPECT_EQ(bash_calls.load(), 0);

    auto results = h.results();
    bool saw_block = false;
    for (const auto& [name, result] : results) {
        if (name == "bash") {
            saw_block = true;
            EXPECT_FALSE(result.success);
            EXPECT_NE(result.output.find("Shell write blocked"), std::string::npos);
        }
    }
    EXPECT_TRUE(saw_block);

    fs::remove_all(cwd);
}
