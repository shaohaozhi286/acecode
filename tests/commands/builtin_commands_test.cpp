#include <gtest/gtest.h>

#include "commands/builtin_commands.hpp"
#include "commands/command_registry.hpp"
#include "commands/desktop_command.hpp"
#include "config/config.hpp"
#include "permissions.hpp"
#include "provider/provider_factory.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "tool/mcp_manager.hpp"
#include "tool/tool_executor.hpp"
#include "utils/token_tracker.hpp"
#include "utils/paths.hpp"
#include "../agent_loop/stub_provider.hpp"

#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <random>
#include <thread>

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

acecode::ChatMessage identified_user_message(
    const std::string& id,
    const std::string& content) {
    auto msg = message("user", content);
    msg.uuid = id;
    msg.timestamp = "2026-07-23T00:00:00Z";
    return msg;
}

void write_text_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    ofs << content;
}

std::string read_text_file(const fs::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    std::string content;
    std::getline(ifs, content, '\0');
    return content;
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

std::vector<std::string> mcp_helper_args(std::initializer_list<std::string> args) {
    return std::vector<std::string>(args.begin(), args.end());
}

acecode::AppConfig mcp_config_with_stdio_server(const std::string& name,
                                                std::vector<std::string> args) {
    acecode::AppConfig cfg;
    acecode::McpServerConfig server;
    server.transport = acecode::McpTransport::Stdio;
    server.command = ACECODE_MCP_STDIO_TEST_SERVER_PATH;
    server.args = std::move(args);
    cfg.mcp_servers[name] = std::move(server);
    return cfg;
}

class McpCommandHarness {
public:
    explicit McpCommandHarness(const std::string& hint)
        : cwd_(temp_cwd(hint))
        , loop_([this] {
                    std::lock_guard<std::mutex> lk(provider_slot_.mu);
                    return provider_slot_.provider;
                },
                tools_,
                acecode::AgentCallbacks{},
                cwd_.string(),
                perms_) {
        acecode::register_builtin_commands(registry_);
    }

    ~McpCommandHarness() {
        mcp_.shutdown();
        loop_.shutdown();
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
        ctx.mcp_manager = &mcp_;
        ctx.tools = &tools_;
        ctx.command_registry = &registry_;
        ctx.cwd = cwd_.string();
        return ctx;
    }

    bool dispatch(const std::string& text) {
        auto ctx = context();
        return registry_.dispatch(text, ctx);
    }

    std::string last_system_message() const {
        if (state_.conversation.empty()) return "";
        return state_.conversation.back().content;
    }

    acecode::TuiState state_;
    acecode::CommandRegistry registry_;
    acecode::SessionEntry::ProviderSlot provider_slot_;
    acecode::ToolExecutor tools_;
    acecode::McpManager mcp_;
    acecode::PermissionManager perms_;
    acecode::AppConfig config_;
    acecode::TokenTracker tracker_;
    fs::path cwd_;
    acecode::AgentLoop loop_;
};

class CommandTestProvider : public acecode_test::StubLlmProvider {
public:
    acecode::ChatResponse chat(
        const std::vector<acecode::ChatMessage>& messages,
        const std::vector<acecode::ToolDef>& tools) override {
        {
            std::lock_guard<std::mutex> lk(side_mu_);
            ++side_calls_;
            side_tools_empty_ = side_tools_empty_ && tools.empty();
            if (!messages.empty()) {
                side_prompts_.push_back(messages.back().content);
            }
        }
        side_cv_.notify_all();
        acecode::ChatResponse response;
        response.content = "detached answer";
        response.finish_reason = "stop";
        return response;
    }

    bool wait_for_side_calls(int count) {
        std::unique_lock<std::mutex> lk(side_mu_);
        return side_cv_.wait_for(lk, std::chrono::seconds(5), [this, count] {
            return side_calls_ >= count;
        });
    }

    int side_calls() const {
        std::lock_guard<std::mutex> lk(side_mu_);
        return side_calls_;
    }

    bool side_tools_empty() const {
        std::lock_guard<std::mutex> lk(side_mu_);
        return side_tools_empty_;
    }

    std::vector<std::string> side_prompts() const {
        std::lock_guard<std::mutex> lk(side_mu_);
        return side_prompts_;
    }

private:
    mutable std::mutex side_mu_;
    std::condition_variable side_cv_;
    int side_calls_ = 0;
    bool side_tools_empty_ = true;
    std::vector<std::string> side_prompts_;
};

class TurnCommandHarness {
public:
    TurnCommandHarness()
        : cwd_(temp_cwd("turn"))
        , provider_(std::make_shared<CommandTestProvider>())
        , loop_(
              [this] { return provider_; },
              tools_,
              callbacks(),
              cwd_.string(),
              perms_) {
        acecode::register_builtin_commands(registry_);
    }

    ~TurnCommandHarness() {
        loop_.shutdown();
        fs::remove_all(cwd_);
    }

    acecode::CommandContext context() {
        return {
            state_,
            loop_,
            nullptr,
            config_,
            tracker_,
            perms_,
        };
    }

    bool dispatch(const std::string& text) {
        auto ctx = context();
        return registry_.dispatch(text, ctx);
    }

    std::string wait_for_active_turn() {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            auto id = loop_.active_turn_id();
            if (!id.empty()) return id;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return loop_.active_turn_id();
    }

    bool wait_until_idle() {
        std::unique_lock<std::mutex> lk(mu_);
        return cv_.wait_for(lk, std::chrono::seconds(10), [this] {
            return saw_busy_ && !busy_;
        });
    }

    bool wait_for_tui_message(const std::string& needle) {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> lk(state_.mu);
                for (const auto& message : state_.conversation) {
                    if (message.content.find(needle) != std::string::npos) {
                        return true;
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    }

    acecode::AgentCallbacks callbacks() {
        acecode::AgentCallbacks value;
        value.on_busy_changed = [this](bool busy) {
            std::lock_guard<std::mutex> lk(mu_);
            busy_ = busy;
            saw_busy_ = saw_busy_ || busy;
            cv_.notify_all();
        };
        return value;
    }

    fs::path cwd_;
    std::shared_ptr<CommandTestProvider> provider_;
    acecode::ToolExecutor tools_;
    acecode::PermissionManager perms_;
    acecode::TuiState state_;
    acecode::AppConfig config_;
    acecode::TokenTracker tracker_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool busy_ = false;
    bool saw_busy_ = false;
    acecode::AgentLoop loop_;
    acecode::CommandRegistry registry_;
};

} // namespace

TEST(BuiltinCommands, NewIsRegisteredAsClearAlias) {
    acecode::CommandRegistry registry;

    acecode::register_builtin_commands(registry);

    ASSERT_TRUE(registry.has_command("clear"));
    ASSERT_TRUE(registry.has_command("new"));
    EXPECT_EQ(registry.commands().at("new").description, "Alias for /clear");
}

TEST(BuiltinCommands, TurnBtwAndSideCommandsAreRegistered) {
    acecode::CommandRegistry registry;
    acecode::register_builtin_commands(registry);

    EXPECT_TRUE(registry.has_command("turn"));
    EXPECT_TRUE(registry.has_command("btw"));
    EXPECT_TRUE(registry.has_command("side"));
}

TEST(BuiltinCommands, ForkCommandIsRegisteredAndListedInHelp) {
    ResumeCommandHarness harness("fork_help");

    ASSERT_TRUE(harness.registry_.has_command("fork"));
    EXPECT_EQ(harness.registry_.commands().at("fork").description,
              "Fork from a previous user turn");
    ASSERT_TRUE(harness.dispatch("/help"));
    ASSERT_FALSE(harness.state_.conversation.empty());
    EXPECT_NE(harness.state_.conversation.back().content.find("/fork"),
              std::string::npos);
}

TEST(BuiltinCommands, ForkAndRewindOpenTheSameTargetListWithDifferentOperations) {
    ResumeCommandHarness harness("fork_picker");
    auto append = [&](const acecode::ChatMessage& msg) {
        harness.loop_.push_message(msg);
        harness.sm_.on_message(msg);
    };
    append(identified_user_message("u1", "first prompt"));
    append(message("assistant", "first answer"));
    append(identified_user_message("u2", "second prompt"));

    ASSERT_TRUE(harness.dispatch("/rewind"));
    ASSERT_TRUE(harness.state_.rewind_picker_active);
    EXPECT_EQ(
        harness.state_.rewind_picker_operation,
        acecode::TuiState::RewindPickerOperation::Rewind);
    const auto rewind_items = harness.state_.rewind_items;

    ASSERT_TRUE(harness.dispatch("/fork"));
    ASSERT_TRUE(harness.state_.rewind_picker_active);
    EXPECT_EQ(
        harness.state_.rewind_picker_operation,
        acecode::TuiState::RewindPickerOperation::Fork);
    ASSERT_EQ(harness.state_.rewind_items.size(), rewind_items.size());
    for (size_t i = 0; i < rewind_items.size(); ++i) {
        EXPECT_EQ(harness.state_.rewind_items[i].message_index,
                  rewind_items[i].message_index);
        EXPECT_EQ(harness.state_.rewind_items[i].message_uuid,
                  rewind_items[i].message_uuid);
        EXPECT_EQ(harness.state_.rewind_items[i].preview,
                  rewind_items[i].preview);
        EXPECT_EQ(harness.state_.rewind_items[i].display,
                  rewind_items[i].display);
    }

    EXPECT_TRUE(acecode::TuiState::rewind_target_uses_mode_picker(
        acecode::TuiState::RewindPickerOperation::Rewind, true));
    EXPECT_FALSE(acecode::TuiState::rewind_target_uses_mode_picker(
        acecode::TuiState::RewindPickerOperation::Fork, true));
    EXPECT_FALSE(acecode::TuiState::rewind_target_uses_mode_picker(
        acecode::TuiState::RewindPickerOperation::Fork, false));
}

TEST(BuiltinCommands, ForkCallbackCreatesBeforeTargetBranchWithoutRestoringFiles) {
    ResumeCommandHarness harness("fork_callback");
    auto append = [&](const acecode::ChatMessage& msg) {
        harness.loop_.push_message(msg);
        harness.sm_.on_message(msg);
    };

    append(identified_user_message("u1", "first prompt"));
    append(message("assistant", "first answer"));
    append(identified_user_message("u2", "change direction"));

    const fs::path tracked = harness.cwd_ / "tracked.txt";
    write_text_file(tracked, "before\n");
    harness.sm_.begin_user_turn_checkpoint("u2");
    harness.sm_.track_file_write_before(tracked.string());
    write_text_file(tracked, "current\n");

    append(message("assistant", "old branch answer"));
    const std::string original_session_id =
        harness.sm_.current_session_id();
    ASSERT_FALSE(original_session_id.empty());

    ASSERT_TRUE(harness.dispatch("/fork"));
    ASSERT_TRUE(harness.state_.rewind_picker_active);
    ASSERT_FALSE(harness.state_.rewind_items.empty());
    const auto selected = harness.state_.rewind_items.front();
    EXPECT_EQ(selected.message_uuid, "u2");
    ASSERT_TRUE(selected.can_restore_code);
    ASSERT_TRUE(harness.state_.rewind_callback);

    {
        std::lock_guard<std::mutex> lk(harness.state_.mu);
        auto callback = harness.state_.rewind_callback;
        callback(
            selected,
            acecode::TuiState::RewindRestoreMode::ConversationOnly);
    }

    EXPECT_EQ(read_text_file(tracked), "current\n");
    EXPECT_NE(harness.sm_.current_session_id(), original_session_id);
    ASSERT_EQ(harness.loop_.messages().size(), 2u);
    EXPECT_EQ(harness.loop_.messages()[0].content, "first prompt");
    EXPECT_EQ(harness.loop_.messages()[1].content, "first answer");
    EXPECT_EQ(harness.state_.input_text, "change direction");
    EXPECT_EQ(harness.state_.input_cursor,
              harness.state_.input_text.size());
    EXPECT_FALSE(harness.state_.is_waiting);
    ASSERT_FALSE(harness.state_.conversation.empty());
    EXPECT_NE(
        harness.state_.conversation.back().content.find(
            "Conversation forked before:"),
        std::string::npos);

    const auto project_dir =
        acecode::SessionStorage::get_project_dir(harness.cwd_.string());
    const auto original_messages = acecode::SessionStorage::load_messages(
        acecode::SessionStorage::session_path(
            project_dir, original_session_id));
    ASSERT_GE(original_messages.size(), 4u);
    EXPECT_EQ(original_messages.back().content, "old branch answer");
}

TEST(BuiltinCommands, DesktopCommandIsRegisteredAndListedInHelp) {
    ResumeCommandHarness harness("desktop_help");

    ASSERT_TRUE(harness.registry_.has_command("desktop"));
    EXPECT_EQ(harness.registry_.commands().at("desktop").description,
              "Open ACECode Desktop");
    ASSERT_TRUE(harness.dispatch("/help"));
    ASSERT_FALSE(harness.state_.conversation.empty());
    EXPECT_NE(harness.state_.conversation.back().content.find("/desktop"),
              std::string::npos);
}

TEST(CommandRegistry, ObserverRunsOnceOnlyForRecognizedObservedDispatches) {
    ResumeCommandHarness harness("command_observer");
    std::vector<std::string> observed;
    auto observed_context = harness.context();
    observed_context.on_command_recognized =
        [&observed](const std::string& name) { observed.push_back(name); };

    EXPECT_TRUE(harness.registry_.dispatch("/help", observed_context));
    EXPECT_EQ(observed, (std::vector<std::string>{"help"}));

    EXPECT_TRUE(harness.registry_.dispatch(
        "/definitely-not-a-command", observed_context));
    EXPECT_EQ(observed, (std::vector<std::string>{"help"}));

    auto unobserved_context = harness.context();
    EXPECT_TRUE(harness.registry_.dispatch("/help", unobserved_context));
    EXPECT_EQ(observed, (std::vector<std::string>{"help"}));
}

TEST(BuiltinCommands, DesktopCommandValidatesArgumentsAndReportsLaunchResult) {
    ResumeCommandHarness harness("desktop_launch_result");
    int launch_calls = 0;
    const fs::path desktop_path = harness.cwd_ / "acecode-desktop.exe";
    acecode::desktop::DesktopOpenRequest captured_request;
    acecode::register_desktop_command(
        harness.registry_,
        [&](const acecode::desktop::DesktopOpenRequest& request) {
            ++launch_calls;
            captured_request = request;
            return acecode::DesktopLaunchResult{true, desktop_path, {}};
        });

    ASSERT_TRUE(harness.dispatch("/desktop unexpected"));
    ASSERT_FALSE(harness.state_.conversation.empty());
    EXPECT_EQ(harness.state_.conversation.back().content, "Usage: /desktop");
    EXPECT_EQ(launch_calls, 0);

    ASSERT_TRUE(harness.dispatch("/desktop"));
    EXPECT_EQ(launch_calls, 1);
    EXPECT_EQ(captured_request.cwd, harness.cwd_.string());
    EXPECT_FALSE(captured_request.session_id.empty());
    EXPECT_EQ(captured_request.session_id, harness.sm_.current_session_id());
    EXPECT_NE(harness.state_.conversation.back().content.find(
                  "ACECode Desktop started:"),
              std::string::npos);
    EXPECT_NE(harness.state_.conversation.back().content.find(
                  "acecode-desktop.exe"),
              std::string::npos);

    acecode::register_desktop_command(
        harness.registry_,
        [](const acecode::desktop::DesktopOpenRequest&) {
            return acecode::DesktopLaunchResult{
                false, {}, "synthetic launch failure"};
        });
    ASSERT_TRUE(harness.dispatch("/desktop"));
    EXPECT_EQ(harness.state_.conversation.back().content,
              "Failed to start ACECode Desktop: synthetic launch failure");
}

TEST(BuiltinCommands, TurnCommandSteersTheCurrentlyRunningTurn) {
    TurnCommandHarness h;
    h.provider_->set_latency_ms(100);
    h.provider_->push_text("intermediate");
    h.provider_->push_text("final");
    h.loop_.submit("start");
    ASSERT_FALSE(h.wait_for_active_turn().empty());

    ASSERT_TRUE(h.dispatch("/turn keep the public API stable"));
    ASSERT_FALSE(h.state_.conversation.empty());
    EXPECT_EQ(
        h.state_.conversation.back().content,
        "Guidance accepted for the active turn.");
    ASSERT_TRUE(h.wait_until_idle());
    ASSERT_EQ(h.provider_->turn_count(), 2);

    const auto request = h.provider_->messages_for_turn(1);
    ASSERT_FALSE(request.empty());
    EXPECT_EQ(request.back().role, "user");
    EXPECT_NE(
        request.back().content.find("keep the public API stable"),
        std::string::npos);
}

TEST(BuiltinCommands, SideQuestionAliasesValidateTheirOwnUsage) {
    TurnCommandHarness h;
    ASSERT_TRUE(h.dispatch("/btw"));
    ASSERT_TRUE(h.dispatch("/side"));
    ASSERT_GE(h.state_.conversation.size(), 2u);
    EXPECT_EQ(
        h.state_.conversation[h.state_.conversation.size() - 2].content,
        "Usage: /btw <question>");
    EXPECT_EQ(
        h.state_.conversation.back().content,
        "Usage: /side <question>");
}

TEST(BuiltinCommands, BtwAndSideRunEquivalentDetachedAsyncQuestions) {
    TurnCommandHarness h;
    h.provider_->push_text("main answer");
    h.loop_.submit("establish context");
    ASSERT_TRUE(h.wait_until_idle());
    const auto main_message_count = h.loop_.messages().size();

    ASSERT_TRUE(h.dispatch("/btw explain one"));
    ASSERT_TRUE(h.provider_->wait_for_side_calls(1));
    ASSERT_TRUE(h.wait_for_tui_message("[/btw] detached answer"));

    ASSERT_TRUE(h.dispatch("/side explain two"));
    ASSERT_TRUE(h.provider_->wait_for_side_calls(2));
    ASSERT_TRUE(h.wait_for_tui_message("[/side] detached answer"));

    EXPECT_EQ(h.provider_->side_calls(), 2);
    EXPECT_TRUE(h.provider_->side_tools_empty());
    const auto prompts = h.provider_->side_prompts();
    ASSERT_EQ(prompts.size(), 2u);
    EXPECT_NE(prompts[0].find("explain one"), std::string::npos);
    EXPECT_NE(prompts[1].find("explain two"), std::string::npos);
    EXPECT_EQ(h.loop_.messages().size(), main_message_count);
}

TEST(BuiltinCommands, FeedbackRejectsInvalidUpgradeUrlLocally) {
    ResumeCommandHarness harness("feedback_invalid_upgrade_url");
    harness.config_.upgrade.base_url = "ftp://bad.example.test/";

    EXPECT_TRUE(harness.dispatch("/feedback upload failed after resume"));
    ASSERT_FALSE(harness.state_.conversation.empty());
    const auto& msg = harness.state_.conversation.back();
    EXPECT_EQ(msg.role, "system");
    EXPECT_NE(msg.content.find("upgrade.base_url"), std::string::npos);
    EXPECT_NE(msg.content.find("http or https"), std::string::npos);
}

TEST(BuiltinCommands, McpCommandShowsStartingState) {
    McpCommandHarness h("mcp_starting");
    auto cfg = mcp_config_with_stdio_server(
        "slow",
        mcp_helper_args({"--delay-ms", "400", "--tool", "echo"}));
    ASSERT_TRUE(h.mcp_.connect_all(cfg));
    h.mcp_.start_async(h.tools_);
    ASSERT_TRUE(h.mcp_.has_starting_servers());

    ASSERT_TRUE(h.dispatch("/mcp"));

    const std::string out = h.last_system_message();
    EXPECT_NE(out.find("slow"), std::string::npos);
    EXPECT_NE(out.find("[starting]"), std::string::npos);
}

TEST(BuiltinCommands, McpCommandShowsFailedStateAndError) {
    McpCommandHarness h("mcp_failed");
    auto cfg = mcp_config_with_stdio_server(
        "bad",
        mcp_helper_args({"--fail-initialize"}));
    ASSERT_TRUE(h.mcp_.connect_all(cfg));
    h.mcp_.start_async(h.tools_);
    ASSERT_TRUE(h.mcp_.wait_for_startup_settled(std::chrono::seconds(5)));

    ASSERT_TRUE(h.dispatch("/mcp"));

    const std::string out = h.last_system_message();
    EXPECT_NE(out.find("bad"), std::string::npos);
    EXPECT_NE(out.find("[failed]"), std::string::npos);
    EXPECT_NE(out.find("error=initialization failed"), std::string::npos);
}

TEST(BuiltinCommands, McpCommandShowsDisabledState) {
    McpCommandHarness h("mcp_disabled");
    auto cfg = mcp_config_with_stdio_server(
        "off",
        mcp_helper_args({"--no-tools"}));
    ASSERT_TRUE(h.mcp_.connect_all(cfg));
    h.mcp_.start_async(h.tools_);
    ASSERT_TRUE(h.mcp_.wait_for_startup_settled(std::chrono::seconds(5)));
    ASSERT_TRUE(h.mcp_.disable("off", h.tools_));

    ASSERT_TRUE(h.dispatch("/mcp"));

    const std::string out = h.last_system_message();
    EXPECT_NE(out.find("off"), std::string::npos);
    EXPECT_NE(out.find("[disabled]"), std::string::npos);
}

TEST(BuiltinCommands, McpDisableDoesNotDeadlockWithStatusCallback) {
    McpCommandHarness h("mcp_disable_callback");
    auto cfg = mcp_config_with_stdio_server(
        "off",
        mcp_helper_args({"--no-tools"}));
    ASSERT_TRUE(h.mcp_.connect_all(cfg));
    h.mcp_.start_async(h.tools_);
    ASSERT_TRUE(h.mcp_.wait_for_startup_settled(std::chrono::seconds(5)));
    h.mcp_.set_status_callback([&h](const acecode::McpServerInfo&) {
        std::lock_guard<std::mutex> lk(h.state_.mu);
        h.state_.chat_follow_tail = true;
    });

    ASSERT_TRUE(h.dispatch("/mcp disable off"));

    const std::string out = h.last_system_message();
    EXPECT_NE(out.find("Disabled MCP server 'off'."), std::string::npos);
}

TEST(BuiltinCommands, McpListShowsNoToolsForConnectedEmptyServer) {
    McpCommandHarness h("mcp_no_tools");
    auto cfg = mcp_config_with_stdio_server(
        "empty",
        mcp_helper_args({"--no-tools"}));
    ASSERT_TRUE(h.mcp_.connect_all(cfg));
    h.mcp_.start_async(h.tools_);
    ASSERT_TRUE(h.mcp_.wait_for_startup_settled(std::chrono::seconds(5)));

    ASSERT_TRUE(h.dispatch("/mcp list"));

    const std::string out = h.last_system_message();
    EXPECT_NE(out.find("empty  [connected]"), std::string::npos);
    EXPECT_NE(out.find("(no tools registered)"), std::string::npos);
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

TEST(BuiltinCommands, ModeWithoutArgumentsOpensPickerAtCurrentMode) {
    ResumeCommandHarness h("mode_picker_open");
    h.perms_.set_mode(acecode::PermissionMode::AcceptEdits);

    ASSERT_TRUE(h.dispatch("/mode"));

    std::lock_guard<std::mutex> lk(h.state_.mu);
    ASSERT_TRUE(h.state_.mode_picker_open);
    ASSERT_EQ(h.state_.mode_picker_options.size(), 4u);
    EXPECT_EQ(h.state_.mode_picker_selected, 1);
    EXPECT_EQ(h.state_.mode_picker_options[1].mode,
              acecode::PermissionMode::AcceptEdits);
    EXPECT_TRUE(h.state_.mode_picker_options[1].is_current);
    EXPECT_TRUE(h.state_.mode_picker_callback);
}

TEST(BuiltinCommands, ModePickerSelectionUsesCurrentSessionTransition) {
    ResumeCommandHarness h("mode_picker_select");
    h.perms_.set_mode(acecode::PermissionMode::AcceptEdits);

    ASSERT_TRUE(h.dispatch("/mode"));
    {
        std::lock_guard<std::mutex> lk(h.state_.mu);
        ASSERT_TRUE(h.state_.mode_picker_callback);
        auto callback = h.state_.mode_picker_callback;
        h.state_.mode_picker_open = false;
        h.state_.mode_picker_options.clear();
        h.state_.mode_picker_callback = nullptr;
        callback(acecode::PermissionMode::Plan);
    }

    EXPECT_EQ(h.perms_.mode(), acecode::PermissionMode::Plan);
    EXPECT_EQ(h.perms_.pre_plan_mode(), acecode::PermissionMode::AcceptEdits);
    EXPECT_EQ(h.sm_.current_permission_mode(), "plan");
    EXPECT_EQ(h.sm_.current_pre_plan_permission_mode(), "accept-edits");
    EXPECT_TRUE(fs::exists(h.sm_.current_plan_file_path()));
    ASSERT_FALSE(h.state_.conversation.empty());
    EXPECT_NE(h.state_.conversation.back().content.find("Permission mode: plan"),
              std::string::npos);
}

TEST(BuiltinCommands, ClosingModePickerWithoutSelectionHasNoSideEffects) {
    ResumeCommandHarness h("mode_picker_cancel");
    h.perms_.set_mode(acecode::PermissionMode::AcceptEdits);
    const auto before_default = h.config_.default_permission_mode;

    ASSERT_TRUE(h.dispatch("/mode"));
    {
        std::lock_guard<std::mutex> lk(h.state_.mu);
        h.state_.mode_picker_open = false;
        h.state_.mode_picker_options.clear();
        h.state_.mode_picker_selected = 0;
        h.state_.mode_picker_callback = nullptr;
    }

    EXPECT_EQ(h.perms_.mode(), acecode::PermissionMode::AcceptEdits);
    EXPECT_EQ(h.config_.default_permission_mode, before_default);
}

TEST(BuiltinCommands, ModeDirectArgumentStillSwitchesWithoutPicker) {
    ResumeCommandHarness h("mode_direct_yolo");

    ASSERT_TRUE(h.dispatch("/mode yolo"));

    EXPECT_EQ(h.perms_.mode(), acecode::PermissionMode::Yolo);
    EXPECT_EQ(h.sm_.current_permission_mode(), "yolo");
    std::lock_guard<std::mutex> lk(h.state_.mu);
    EXPECT_FALSE(h.state_.mode_picker_open);
    EXPECT_TRUE(h.state_.mode_picker_options.empty());
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

TEST(BuiltinCommands, ResumeByStableIdRefreshesCanonicalRuntimeState) {
    ResumeCommandHarness h("resume_stable_id");
    auto ctx = h.context();

    ASSERT_TRUE(acecode::resume_session_by_id(ctx, h.target_session_id_));
    EXPECT_EQ(h.sm_.current_session_id(), h.target_session_id_);
    h.expect_resumed_runtime_state();
}

TEST(BuiltinCommands, ResumeByStableIdRejectsMissingSession) {
    ResumeCommandHarness h("resume_missing_stable_id");
    auto ctx = h.context();

    EXPECT_FALSE(acecode::resume_session_by_id(ctx, "missing-session"));
    ASSERT_FALSE(h.state_.conversation.empty());
    EXPECT_NE(h.state_.conversation.back().content.find("not found"),
              std::string::npos);
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
