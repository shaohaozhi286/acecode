#include <gtest/gtest.h>

#include "commands/builtin_commands.hpp"
#include "commands/command_registry.hpp"
#include "config/config.hpp"
#include "permissions.hpp"
#include "provider/provider_factory.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "tool/tool_executor.hpp"
#include "utils/token_tracker.hpp"

#include <filesystem>
#include <memory>
#include <random>

namespace fs = std::filesystem;

namespace {

fs::path temp_cwd(const std::string& hint) {
    auto dir = fs::temp_directory_path() /
        ("acecode_builtin_commands_" + hint + "_" + std::to_string(std::random_device{}()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    fs::remove_all(acecode::SessionStorage::get_project_dir(dir.string()));
    return dir;
}

acecode::ChatMessage message(const std::string& role, const std::string& content) {
    acecode::ChatMessage msg;
    msg.role = role;
    msg.content = content;
    return msg;
}

acecode::ModelProfile model_profile(const std::string& name,
                                    const std::string& model,
                                    int context_window) {
    acecode::ModelProfile profile;
    profile.name = name;
    profile.provider = "openai";
    profile.base_url = "http://127.0.0.1:9/v1";
    profile.api_key = "test-key";
    profile.model = model;
    profile.context_window = context_window;
    return profile;
}

class ResumeCommandHarness {
public:
    explicit ResumeCommandHarness(const std::string& hint)
        : cwd_(temp_cwd(hint))
        , loop_([this] {
                    std::lock_guard<std::mutex> lk(provider_slot_.mu);
                    return provider_slot_.provider;
                },
                tools_,
                acecode::AgentCallbacks{},
                cwd_.string(),
                perms_) {
        config_.context_window = 128000;
        config_.saved_models = {
            model_profile("default", "gpt-default", 128000),
            model_profile("mini", "gpt-mini", 64000),
        };
        config_.default_model_name = "default";
        {
            std::lock_guard<std::mutex> lk(provider_slot_.mu);
            provider_slot_.provider = acecode::create_provider_from_entry(
                config_.saved_models.front(), &config_);
        }

        create_target_session();
        sm_.start_session(cwd_.string(), "openai", "gpt-default");
        loop_.set_session_manager(&sm_);
        loop_.set_context_window(config_.context_window);
        acecode::register_builtin_commands(registry_);
    }

    ~ResumeCommandHarness() {
        loop_.shutdown();
        fs::remove_all(acecode::SessionStorage::get_project_dir(cwd_.string()));
        fs::remove_all(cwd_);
    }

    acecode::CommandContext context() {
        acecode::CommandContext ctx{
            state_,
            loop_,
            &provider_slot_,
            config_,
            tracker_,
            perms_,
        };
        ctx.session_manager = &sm_;
        ctx.tools = &tools_;
        ctx.command_registry = &registry_;
        ctx.cwd = cwd_.string();
        return ctx;
    }

    bool dispatch(const std::string& text) {
        auto ctx = context();
        return registry_.dispatch(text, ctx);
    }

    void expect_resumed_runtime_state() const {
        EXPECT_EQ(config_.context_window, 64000);
        EXPECT_EQ(tracker_.last_prompt_tokens(), 8000);
        EXPECT_EQ(tracker_.prompt_tokens(), 8000);
        EXPECT_EQ(state_.token_status, "8.0k/64.0k");
        EXPECT_EQ(state_.token_percent, 13);
        EXPECT_NE(state_.status_line.find("[openai]"), std::string::npos);
        EXPECT_NE(state_.status_line.find("gpt-mini"), std::string::npos);
    }

    acecode::TuiState state_;
    acecode::CommandRegistry registry_;
    acecode::SessionManager sm_;
    acecode::SessionEntry::ProviderSlot provider_slot_;
    acecode::ToolExecutor tools_;
    acecode::PermissionManager perms_;
    acecode::AppConfig config_;
    acecode::TokenTracker tracker_;
    fs::path cwd_;
    std::string target_session_id_;
    acecode::AgentLoop loop_;

private:
    void create_target_session() {
        acecode::SessionManager writer;
        writer.start_session(cwd_.string(), "openai", "gpt-mini", "", "mini");
        writer.on_message(message("user", "first"));
        writer.on_message(message("assistant", "reply"));
        target_session_id_ = writer.current_session_id();
        ASSERT_FALSE(target_session_id_.empty());

        acecode::TokenUsage usage;
        usage.prompt_tokens = 8000;
        usage.completion_tokens = 1200;
        usage.total_tokens = 9200;
        usage.has_data = true;
        writer.record_token_usage(usage);
        writer.finalize();
    }
};

} // namespace

TEST(BuiltinCommands, NewIsRegisteredAsClearAlias) {
    acecode::CommandRegistry registry;

    acecode::register_builtin_commands(registry);

    ASSERT_TRUE(registry.has_command("clear"));
    ASSERT_TRUE(registry.has_command("new"));
    EXPECT_EQ(registry.commands().at("new").description, "Alias for /clear");
}

TEST(BuiltinCommands, PlanCommandEntersPlanModeAndCreatesPlanFile) {
    ResumeCommandHarness h("plan_command");
    h.perms_.set_mode(acecode::PermissionMode::AcceptEdits);

    ASSERT_TRUE(h.dispatch("/plan"));

    EXPECT_EQ(h.perms_.mode(), acecode::PermissionMode::Plan);
    EXPECT_EQ(h.perms_.pre_plan_mode(), acecode::PermissionMode::AcceptEdits);
    EXPECT_EQ(h.sm_.current_permission_mode(), "plan");
    EXPECT_EQ(h.sm_.current_pre_plan_permission_mode(), "accept-edits");
    const std::string plan_file = h.sm_.current_plan_file_path();
    ASSERT_FALSE(plan_file.empty());
    EXPECT_TRUE(fs::exists(plan_file));

    std::lock_guard<std::mutex> lk(h.state_.mu);
    ASSERT_FALSE(h.state_.conversation.empty());
    EXPECT_NE(h.state_.conversation.back().content.find("Plan mode enabled"), std::string::npos);
    EXPECT_FALSE(h.state_.is_waiting);
}

TEST(BuiltinCommands, ResumeByNumberRefreshesModelAndTokenState) {
    ResumeCommandHarness h("resume_number");

    ASSERT_TRUE(h.dispatch("/resume 1"));

    h.expect_resumed_runtime_state();
}

TEST(BuiltinCommands, ResumePickerRefreshesModelAndTokenState) {
    ResumeCommandHarness h("resume_picker");

    ASSERT_TRUE(h.dispatch("/resume"));
    ASSERT_TRUE(h.state_.resume_callback);

    {
        std::lock_guard<std::mutex> lk(h.state_.mu);
        h.state_.resume_callback(h.target_session_id_);
    }

    h.expect_resumed_runtime_state();
}
