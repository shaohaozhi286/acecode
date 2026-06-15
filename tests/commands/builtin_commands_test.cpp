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
#include "utils/paths.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <random>

#ifdef _WIN32
#include <stdlib.h>
#endif

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
constexpr const char* kHomeEnvName = "USERPROFILE";
#else
constexpr const char* kHomeEnvName = "HOME";
#endif

void set_env_value(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void clear_env_value(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

class ScopedHomeOverride {
public:
    explicit ScopedHomeOverride(const fs::path& home) : home_(home) {
        acecode::reset_run_mode_for_test();
        if (const char* existing = std::getenv(kHomeEnvName)) {
            previous_ = existing;
            had_previous_ = true;
        }
        fs::create_directories(home_);
        set_env_value(kHomeEnvName, home_.string());
    }

    ~ScopedHomeOverride() {
        if (had_previous_) {
            set_env_value(kHomeEnvName, previous_);
        } else {
            clear_env_value(kHomeEnvName);
        }
        acecode::reset_run_mode_for_test();
        std::error_code ec;
        fs::remove_all(home_, ec);
    }

    fs::path config_path() const { return home_ / ".acecode" / "config.json"; }

private:
    fs::path home_;
    std::string previous_;
    bool had_previous_ = false;
};

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

TEST(BuiltinCommands, ModeDefaultPersistsWithoutChangingCurrentSession) {
    ScopedHomeOverride home(fs::temp_directory_path() /
        ("acecode_builtin_commands_home_" + std::to_string(std::random_device{}())));
    ResumeCommandHarness h("mode_default");
    h.perms_.set_mode(acecode::PermissionMode::AcceptEdits);

    ASSERT_TRUE(h.dispatch("/mode default yolo"));

    EXPECT_EQ(h.config_.default_permission_mode, "yolo");
    EXPECT_EQ(h.perms_.mode(), acecode::PermissionMode::AcceptEdits);
    EXPECT_EQ(h.sm_.current_permission_mode(), "default");

    std::ifstream ifs(home.config_path());
    ASSERT_TRUE(ifs.is_open());
    auto saved = nlohmann::json::parse(ifs);
    EXPECT_EQ(saved["default_permission_mode"], "yolo");
}

TEST(BuiltinCommands, ModeCommandSwitchesCurrentSessionToPlan) {
    ResumeCommandHarness h("mode_plan");
    h.perms_.set_mode(acecode::PermissionMode::AcceptEdits);

    ASSERT_TRUE(h.dispatch("/mode plan"));

    EXPECT_EQ(h.perms_.mode(), acecode::PermissionMode::Plan);
    EXPECT_EQ(h.perms_.pre_plan_mode(), acecode::PermissionMode::AcceptEdits);
    EXPECT_EQ(h.sm_.current_permission_mode(), "plan");
    EXPECT_EQ(h.sm_.current_pre_plan_permission_mode(), "accept-edits");
    EXPECT_TRUE(fs::exists(h.sm_.current_plan_file_path()));
    EXPECT_EQ(h.config_.default_permission_mode, "default");
}

TEST(BuiltinCommands, ModelSetDefaultWithoutNameOpensDefaultPickerOnly) {
    ScopedHomeOverride home(fs::temp_directory_path() /
        ("acecode_builtin_commands_home_" + std::to_string(std::random_device{}())));
    ResumeCommandHarness h("model_default_picker");

    ASSERT_TRUE(h.dispatch("/model set-default"));

    {
        std::lock_guard<std::mutex> lk(h.state_.mu);
        ASSERT_TRUE(h.state_.model_picker_open);
        ASSERT_TRUE(h.state_.model_picker_callback);
        h.state_.model_picker_callback("mini");
    }

    EXPECT_EQ(h.config_.default_model_name, "mini");
    auto provider = [&] {
        std::lock_guard<std::mutex> lk(h.provider_slot_.mu);
        return h.provider_slot_.provider;
    }();
    ASSERT_NE(provider, nullptr);
    EXPECT_EQ(provider->model(), "gpt-default")
        << "default picker must not switch the current session model";

    std::ifstream ifs(home.config_path());
    ASSERT_TRUE(ifs.is_open());
    auto saved = nlohmann::json::parse(ifs);
    EXPECT_EQ(saved["default_model_name"], "mini");
}

TEST(BuiltinCommands, ClearAppliesExternalDefaultsToNextLazySessionWithoutEmptyFile) {
    ScopedHomeOverride home(fs::temp_directory_path() /
        ("acecode_builtin_commands_home_" + std::to_string(std::random_device{}())));
    ResumeCommandHarness h("clear_defaults");

    acecode::AppConfig disk = h.config_;
    disk.default_model_name = "mini";
    disk.default_permission_mode = "yolo";
    acecode::save_config(disk);
    const auto before = h.sm_.list_sessions().size();

    ASSERT_TRUE(h.dispatch("/clear"));

    EXPECT_EQ(h.config_.default_model_name, "mini");
    EXPECT_EQ(h.config_.default_permission_mode, "yolo");
    EXPECT_EQ(h.perms_.mode(), acecode::PermissionMode::Yolo);
    EXPECT_EQ(h.sm_.current_permission_mode(), "yolo");
    EXPECT_TRUE(h.sm_.current_session_id().empty());
    EXPECT_EQ(h.sm_.list_sessions().size(), before)
        << "applying defaults after /clear must not create an empty session";

    auto provider = [&] {
        std::lock_guard<std::mutex> lk(h.provider_slot_.mu);
        return h.provider_slot_.provider;
    }();
    ASSERT_NE(provider, nullptr);
    EXPECT_EQ(provider->model(), "gpt-mini");

    h.sm_.on_message(message("user", "new session"));
    const std::string sid = h.sm_.current_session_id();
    ASSERT_FALSE(sid.empty());
    auto meta = acecode::SessionStorage::read_meta(
        acecode::SessionStorage::meta_path(
            acecode::SessionStorage::get_project_dir(h.cwd_.string()), sid));
    EXPECT_EQ(meta.model_preset, "mini");
    EXPECT_EQ(meta.model, "gpt-mini");
    EXPECT_EQ(meta.permission_mode, "yolo");
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
