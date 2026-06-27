#include <gtest/gtest.h>

#include "agent_loop.hpp"
#include "permissions.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "session/tool_result_storage.hpp"
#include "stub_provider.hpp"
#include "tool/file_read_tool.hpp"
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

acecode::ToolImpl medium_read_tool() {
    acecode::ToolDef def;
    def.name = "medium_read";
    def.description = "Return a medium payload that exceeds the default single-result threshold.";
    def.parameters = nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}};

    acecode::ToolImpl impl;
    impl.definition = std::move(def);
    impl.is_read_only = true;
    impl.execute = [](const std::string&, const acecode::ToolContext&) {
        return acecode::ToolResult{std::string(60000, 'm'), true};
    };
    return impl;
}

acecode::ToolImpl bash_threshold_tool() {
    acecode::ToolDef def;
    def.name = "bash";
    def.description = "Mock Bash returning output above the Bash single-result threshold.";
    def.parameters = nlohmann::json{
        {"type", "object"},
        {"properties", nlohmann::json{{"command", nlohmann::json{{"type", "string"}}}}},
    };

    acecode::ToolImpl impl;
    impl.definition = std::move(def);
    impl.is_read_only = true;
    impl.execute = [](const std::string&, const acecode::ToolContext&) {
        return acecode::ToolResult{std::string(40000, 'b'), true};
    };
    return impl;
}

acecode::ToolImpl big_image_tool() {
    acecode::ToolDef def;
    def.name = "big_image";
    def.description = "Return a large payload with an image attachment.";
    def.parameters = nlohmann::json{{"type", "object"}, {"properties", nlohmann::json::object()}};

    acecode::ToolImpl impl;
    impl.definition = std::move(def);
    impl.is_read_only = true;
    impl.execute = [](const std::string&, const acecode::ToolContext&) {
        acecode::ToolResult result{std::string(260000, 'y'), true};
        result.attachments = nlohmann::json::array({
            {
                {"name", "plot.png"},
                {"mime_type", "image/png"},
                {"data_url", "data:image/png;base64,YWJj"},
            },
        });
        return result;
    };
    return impl;
}

class Harness {
public:
    Harness()
        : cwd_(temp_cwd("batch")) {
        sm_.start_session(cwd_.string(), "stub", "stub-1", "sid-large-results");
        tools_.register_tool(bash_threshold_tool());
        tools_.register_tool(medium_read_tool());
        tools_.register_tool(big_read_tool());
        tools_.register_tool(big_image_tool());
        tools_.register_tool(acecode::create_file_read_tool());

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

TEST(AgentLoopToolResultStorage, PersistsBashResultBySingleResultThresholdBelowBatchBudget) {
    Harness h;

    h.provider().push_tool_call("bash", R"({"command":"printf large-output"})", "call-bash");
    h.provider().push_text("done");

    ASSERT_TRUE(h.submit_and_wait("run bash"));

    const auto& messages = h.loop().messages();
    auto tool_it = std::find_if(messages.begin(), messages.end(), [](const acecode::ChatMessage& msg) {
        return msg.role == "tool" && msg.tool_call_id == "call-bash";
    });
    ASSERT_NE(tool_it, messages.end());
    EXPECT_TRUE(acecode::is_persisted_output_message(tool_it->content));
    EXPECT_EQ(tool_it->content.find(std::string(40000, 'b')), std::string::npos);

    const fs::path artifact =
        fs::path(h.session_manager().ensure_tool_results_dir()) / "call-bash.txt";
    ASSERT_TRUE(fs::exists(artifact));
    EXPECT_EQ(fs::file_size(artifact), 40000u);
}

TEST(AgentLoopToolResultStorage, PersistsDefaultToolResultBySingleResultThreshold) {
    Harness h;

    h.provider().push_tool_call("medium_read", "{}", "call-medium");
    h.provider().push_text("done");

    ASSERT_TRUE(h.submit_and_wait("run the medium tool"));

    const auto& messages = h.loop().messages();
    auto tool_it = std::find_if(messages.begin(), messages.end(), [](const acecode::ChatMessage& msg) {
        return msg.role == "tool" && msg.tool_call_id == "call-medium";
    });
    ASSERT_NE(tool_it, messages.end());
    EXPECT_TRUE(acecode::is_persisted_output_message(tool_it->content));

    const fs::path artifact =
        fs::path(h.session_manager().ensure_tool_results_dir()) / "call-medium.txt";
    ASSERT_TRUE(fs::exists(artifact));
    EXPECT_EQ(fs::file_size(artifact), 60000u);
}

TEST(AgentLoopToolResultStorage, RepeatedPersistedFileReadReturnsSavedOutputReference) {
    Harness h;
    const fs::path source = h.cwd() / "large-file.txt";
    {
        std::ofstream ofs(source, std::ios::binary);
        ofs << std::string(60000, 'f') << "\n";
    }

    const std::string args = nlohmann::json{{"file_path", source.string()}}.dump();
    h.provider().push_tool_call("file_read", args, "call-file-read");
    h.provider().push_tool_call("file_read", args, "call-file-read-repeat");
    h.provider().push_text("done");

    ASSERT_TRUE(h.submit_and_wait("read the large file twice"));

    const auto& messages = h.loop().messages();
    auto first = std::find_if(messages.begin(), messages.end(), [](const acecode::ChatMessage& msg) {
        return msg.role == "tool" && msg.tool_call_id == "call-file-read";
    });
    ASSERT_NE(first, messages.end());
    ASSERT_TRUE(acecode::is_persisted_output_message(first->content));
    const std::string saved_path = acecode::persisted_output_filepath(first->content);
    ASSERT_FALSE(saved_path.empty());
    ASSERT_TRUE(fs::exists(saved_path));

    auto repeat = std::find_if(messages.begin(), messages.end(), [](const acecode::ChatMessage& msg) {
        return msg.role == "tool" && msg.tool_call_id == "call-file-read-repeat";
    });
    ASSERT_NE(repeat, messages.end());
    EXPECT_NE(repeat->content.find("File unchanged since last read"), std::string::npos);
    EXPECT_NE(repeat->content.find("Previous file_read tool_call_id: call-file-read"),
              std::string::npos);
    EXPECT_NE(repeat->content.find(saved_path), std::string::npos);
    EXPECT_NE(repeat->content.find("call file_read on that saved output path"),
              std::string::npos);
    EXPECT_EQ(repeat->content.find(std::string(60000, 'f')), std::string::npos);
}

TEST(AgentLoopToolResultStorage, PersistsLargestFreshResultWhenAggregateBudgetExceeded) {
    const fs::path cwd = temp_cwd("aggregate");
    const fs::path tool_results_dir = cwd / "sid-aggregate" / acecode::TOOL_RESULTS_SUBDIR;

    std::vector<acecode::ToolCall> tool_calls = {
        {"call-a", "tool_a", "{}"},
        {"call-b", "tool_b", "{}"},
    };
    std::vector<acecode::ToolResult> results = {
        acecode::ToolResult{std::string(49000, 'a'), true},
        acecode::ToolResult{std::string(48000, 'b'), true},
    };
    std::vector<bool> ready = {true, true};
    acecode::ToolResultReplacementState state;
    acecode::ToolResultBudgetOptions options;
    options.per_batch_budget_bytes = 70000;

    auto budget = acecode::enforce_tool_result_budget(
        tool_calls,
        results,
        ready,
        tool_results_dir.string(),
        state,
        options);

    ASSERT_EQ(budget.newly_replaced.size(), 1u);
    EXPECT_EQ(budget.newly_replaced[0].tool_call_id, "call-a");
    EXPECT_TRUE(acecode::is_persisted_output_message(results[0].output));
    EXPECT_EQ(results[1].output, std::string(48000, 'b'));
    EXPECT_TRUE(state.seen_ids.count("call-a"));
    EXPECT_TRUE(state.seen_ids.count("call-b"));
    EXPECT_TRUE(state.replacements.count("call-a"));
    EXPECT_FALSE(state.replacements.count("call-b"));
    EXPECT_TRUE(fs::exists(tool_results_dir / "call-a.txt"));
    EXPECT_EQ(fs::file_size(tool_results_dir / "call-a.txt"), 49000u);

    fs::remove_all(cwd);
}

TEST(AgentLoopToolResultStorage, SeenInlineResultIsNotRewrittenBySingleResultThreshold) {
    const fs::path cwd = temp_cwd("seen-inline");
    const fs::path tool_results_dir = cwd / "sid-seen" / acecode::TOOL_RESULTS_SUBDIR;
    const std::string original(40000, 's');

    std::vector<acecode::ToolCall> tool_calls = {
        {"call-seen", "bash", R"({"command":"printf cached"})"},
    };
    std::vector<acecode::ToolResult> results = {
        acecode::ToolResult{original, true},
    };
    std::vector<bool> ready = {true};
    acecode::ToolResultReplacementState state;
    state.seen_ids.insert("call-seen");

    auto budget = acecode::enforce_tool_result_budget(
        tool_calls,
        results,
        ready,
        tool_results_dir.string(),
        state);

    EXPECT_TRUE(budget.newly_replaced.empty());
    EXPECT_EQ(results[0].output, original);
    EXPECT_TRUE(state.replacements.empty());
    EXPECT_FALSE(fs::exists(tool_results_dir / "call-seen.txt"));

    fs::remove_all(cwd);
}

TEST(AgentLoopToolResultStorage, LargeToolResultReplacementPreservesAttachments) {
    Harness h;

    h.provider().push_tool_call("big_image", "{}", "call-big-image");
    h.provider().push_text("done");

    ASSERT_TRUE(h.submit_and_wait("run the image tool"));

    const auto& messages = h.loop().messages();
    auto tool_it = std::find_if(messages.begin(), messages.end(), [](const acecode::ChatMessage& msg) {
        return msg.role == "tool" && msg.tool_call_id == "call-big-image";
    });
    ASSERT_NE(tool_it, messages.end());
    EXPECT_TRUE(acecode::is_persisted_output_message(tool_it->content));
    ASSERT_TRUE(tool_it->content_parts.is_array());
    ASSERT_EQ(tool_it->content_parts.size(), 1u);
    EXPECT_EQ(tool_it->content_parts[0]["type"], "image");
    EXPECT_EQ(tool_it->content_parts[0]["attachment"]["name"], "plot.png");
    EXPECT_EQ(tool_it->content_parts[0]["attachment"]["blob_url"].get<std::string>().find(
                  "/api/sessions/sid-large-results/attachments/"),
              0u);

    auto followup_messages = h.provider().messages_for_turn(1);
    auto followup_tool = std::find_if(
        followup_messages.begin(),
        followup_messages.end(),
        [](const acecode::ChatMessage& msg) {
            return msg.role == "tool" && msg.tool_call_id == "call-big-image";
        });
    ASSERT_NE(followup_tool, followup_messages.end());
    ASSERT_TRUE(followup_tool->content_parts.is_array());
    ASSERT_EQ(followup_tool->content_parts.size(), 1u);
    EXPECT_EQ(followup_tool->content_parts[0]["attachment"]["id"],
              tool_it->content_parts[0]["attachment"]["id"]);
}
