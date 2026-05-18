#include <gtest/gtest.h>

#include "agent_loop.hpp"
#include "permissions.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "session/tool_result_storage.hpp"
#include "stub_provider.hpp"
#include "tool/tool_executor.hpp"

#include <chrono>
#include <algorithm>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <random>
#include <thread>

using namespace std::chrono_literals;

namespace fs = std::filesystem;

namespace {

fs::path temp_cwd(const std::string& hint) {
    auto dir = fs::temp_directory_path() /
        ("acecode_agent_tool_result_storage_" + hint + "_" +
         std::to_string(std::random_device{}()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    fs::remove_all(acecode::SessionStorage::get_project_dir(dir.string()));
    return dir;
}

acecode::ToolImpl big_read_tool() {
    acecode::ToolDef def;
    def.name = "big_read";
    def.description = "Return a large read-only payload.";
    def.parameters = nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}};

    acecode::ToolImpl impl;
    impl.definition = std::move(def);
    impl.is_read_only = true;
    impl.execute = [](const std::string&, const acecode::ToolContext&) {
        return acecode::ToolResult{std::string(260000, 'x'), true};
    };
    return impl;
}

class Harness {
public:
    Harness()
        : cwd_(temp_cwd("batch")) {
        sm_.start_session(cwd_.string(), "stub", "stub-1", "sid-large-results");
        tools_.register_tool(big_read_tool());

        acecode::AgentCallbacks cb;
        cb.on_busy_changed = [this](bool busy) {
            std::lock_guard<std::mutex> lk(mu_);
            busy_ = busy;
            if (!busy) cv_.notify_all();
        };

        auto accessor = [this]() -> std::shared_ptr<acecode::LlmProvider> {
            return provider_;
        };
        loop_ = std::make_unique<acecode::AgentLoop>(
            accessor, tools_, cb, cwd_.string(), perms_);
        loop_->set_session_manager(&sm_);
    }

    ~Harness() {
        loop_.reset();
        fs::remove_all(acecode::SessionStorage::get_project_dir(cwd_.string()));
        fs::remove_all(cwd_);
    }

    bool submit_and_wait(const std::string& text) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            busy_ = true;
        }
        loop_->submit(text);
        std::unique_lock<std::mutex> lk(mu_);
        return cv_.wait_for(lk, 5s, [this] { return !busy_; });
    }

    acecode_test::StubLlmProvider& provider() { return *provider_; }
    acecode::AgentLoop& loop() { return *loop_; }
    acecode::SessionManager& session_manager() { return sm_; }
    fs::path cwd() const { return cwd_; }

private:
    fs::path cwd_;
    acecode::PermissionManager perms_;
    acecode::ToolExecutor tools_;
    acecode::SessionManager sm_;
    std::shared_ptr<acecode_test::StubLlmProvider> provider_ =
        std::make_shared<acecode_test::StubLlmProvider>();
    std::unique_ptr<acecode::AgentLoop> loop_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    bool busy_ = false;
};

} // namespace

TEST(AgentLoopToolResultStorage, PersistsLargeToolResultBeforeProviderFollowup) {
    Harness h;

    h.provider().push_tool_call("big_read", "{}", "call-big");
    h.provider().push_text("done");

    ASSERT_TRUE(h.submit_and_wait("run the big tool"));

    const auto& messages = h.loop().messages();
    auto tool_it = std::find_if(messages.begin(), messages.end(), [](const acecode::ChatMessage& msg) {
        return msg.role == "tool" && msg.tool_call_id == "call-big";
    });
    ASSERT_NE(tool_it, messages.end());
    EXPECT_TRUE(acecode::is_persisted_output_message(tool_it->content));
    EXPECT_EQ(tool_it->content.find(std::string(260000, 'x')), std::string::npos);

    auto meta_it = std::find_if(messages.begin(), messages.end(), acecode::is_content_replacement_message);
    ASSERT_NE(meta_it, messages.end());

    const fs::path artifact =
        fs::path(h.session_manager().ensure_tool_results_dir()) / "call-big.txt";
    ASSERT_TRUE(fs::exists(artifact));
    EXPECT_EQ(fs::file_size(artifact), 260000u);

    auto followup_messages = h.provider().messages_for_turn(1);
    auto followup_tool = std::find_if(
        followup_messages.begin(),
        followup_messages.end(),
        [](const acecode::ChatMessage& msg) {
            return msg.role == "tool" && msg.tool_call_id == "call-big";
        });
    ASSERT_NE(followup_tool, followup_messages.end());
    EXPECT_TRUE(acecode::is_persisted_output_message(followup_tool->content));
}
