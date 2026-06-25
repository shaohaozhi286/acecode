#include <gtest/gtest.h>

#include "agent_loop.hpp"
#include "config/config.hpp"
#include "commands/remote_control_command.hpp"
#include "permissions.hpp"
#include "remote_control/remote_control_service.hpp"
#include "session/session_manager.hpp"
#include "session/session_storage.hpp"
#include "tool/tool_executor.hpp"
#include "utils/token_tracker.hpp"

#include <filesystem>
#include <memory>
#include <random>

namespace fs = std::filesystem;

using acecode::CommandRegistry;
using acecode::RemoteControlDisplaySnapshot;
using acecode::format_remote_control_display;

namespace {

fs::path temp_cwd(const std::string& hint) {
    auto dir = fs::temp_directory_path() /
        ("acecode_remote_control_command_" + hint + "_" +
         std::to_string(std::random_device{}()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    fs::remove_all(acecode::SessionStorage::get_project_dir(dir.string()));
    return dir;
}

class RemoteControlCommandHarness {
public:
    explicit RemoteControlCommandHarness(const std::string& hint)
        : cwd_(temp_cwd(hint))
        , loop_([] { return std::shared_ptr<acecode::LlmProvider>{}; },
                tools_,
                acecode::AgentCallbacks{},
                cwd_.string(),
                perms_) {
        sm_.start_session(cwd_.string(), "stub", "model", "sid-" + hint);
        loop_.set_session_manager(&sm_);
        acecode::register_remote_control_command(registry_);
    }

    ~RemoteControlCommandHarness() {
        acecode::rc::remote_control_service().stop();
        loop_.shutdown();
        fs::remove_all(cwd_);
        fs::remove_all(acecode::SessionStorage::get_project_dir(cwd_.string()));
    }

    acecode::CommandContext context() {
        acecode::CommandContext ctx{
            state_,
            loop_,
            nullptr,
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

    std::string last_system_message() const {
        if (state_.conversation.empty()) return {};
        return state_.conversation.back().content;
    }

    acecode::TuiState state_;
    acecode::CommandRegistry registry_;
    acecode::SessionManager sm_;
    acecode::ToolExecutor tools_;
    acecode::PermissionManager perms_;
    acecode::AppConfig config_;
    acecode::TokenTracker tracker_;
    fs::path cwd_;
    acecode::AgentLoop loop_;
};

} // namespace

// 场景:命令注册。期望:/remote-control 与短别名 /rc 同时可用 —— 别名是
// 对外承诺的入口(README 已写明),重构注册逻辑时不能丢。
TEST(RemoteControlCommand, RegistersBothFullNameAndRcAlias) {
    CommandRegistry registry;
    acecode::register_remote_control_command(registry);
    EXPECT_TRUE(registry.has_command("remote-control"));
    EXPECT_TRUE(registry.has_command("rc"));
}

TEST(RemoteControlCommand, RcWithoutDefaultChannelShowsGenericSetup) {
    RemoteControlCommandHarness h("no_default");

    ASSERT_TRUE(h.dispatch("/rc"));
    const std::string text = h.last_system_message();
    EXPECT_NE(text.find("Remote control : OFF"), std::string::npos);
    EXPECT_NE(text.find("remote_control.default_channel"), std::string::npos);
    EXPECT_NE(text.find("manual webhook pairing"), std::string::npos);
}

TEST(RemoteControlCommand, RcWithDefaultChannelAttemptsManifestLoad) {
    RemoteControlCommandHarness h("missing_manifest");
    h.config_.remote_control.default_channel = "chat";
    h.config_.remote_control.channels["chat"].manifest_path =
        (h.cwd_ / "missing-channel-plugin.json").string();

    ASSERT_TRUE(h.dispatch("/rc"));
    const std::string text = h.last_system_message();
    EXPECT_NE(text.find("Failed to load channel plugin 'chat'"), std::string::npos);
}

TEST(RemoteControlCommand, ShowAndOffAreSafeWhenStopped) {
    RemoteControlCommandHarness h("show_off");

    ASSERT_TRUE(h.dispatch("/remote-control show"));
    EXPECT_NE(h.last_system_message().find("Remote control : OFF"), std::string::npos);

    ASSERT_TRUE(h.dispatch("/remote-control off"));
    EXPECT_NE(h.last_system_message().find("Remote control is not running"), std::string::npos);
}

// 场景:/remote-control 状态输出(运行中)。期望:包含入站端点、token header、
// 出站 webhook 与统计行 —— 这是用户给 channel bridge 做配对时照抄的全部信息。
TEST(RemoteControlCommandDisplay, RunningShowsPairingInfo) {
    RemoteControlDisplaySnapshot snap;
    snap.running = true;
    snap.port = 28190;
    snap.token = "abc123";
    snap.outbound_url = "http://127.0.0.1:9000/hook";
    snap.inbound_accepted = 3;
    snap.outbound_sent = 5;

    std::string text = format_remote_control_display(snap);
    EXPECT_NE(text.find("ON"), std::string::npos);
    EXPECT_NE(text.find("http://127.0.0.1:28190/rc/send"), std::string::npos);
    EXPECT_NE(text.find("X-ACECode-RC-Token: abc123"), std::string::npos);
    EXPECT_NE(text.find("http://127.0.0.1:9000/hook"), std::string::npos);
    EXPECT_NE(text.find("in 3 ok"), std::string::npos);
    EXPECT_NE(text.find("out 5 sent"), std::string::npos);
}

// 场景:出站 webhook 未配置时的运行状态。期望:明确提示配置命令而不是空串,
// 用户不看文档也知道下一步敲什么。
TEST(RemoteControlCommandDisplay, RunningWithoutOutboundShowsHint) {
    RemoteControlDisplaySnapshot snap;
    snap.running = true;
    snap.port = 28190;
    snap.token = "t";

    std::string text = format_remote_control_display(snap);
    EXPECT_NE(text.find("/remote-control url"), std::string::npos);
}

// 场景:未运行时的状态输出。期望:显示 OFF、配置端口与启动提示,不显示
// 入站端点(没在听,照抄会误导)。
TEST(RemoteControlCommandDisplay, StoppedShowsOffAndStartHint) {
    RemoteControlDisplaySnapshot snap;
    snap.running = false;
    snap.port = 28190;

    std::string text = format_remote_control_display(snap);
    EXPECT_NE(text.find("OFF"), std::string::npos);
    EXPECT_NE(text.find("/remote-control on"), std::string::npos);
    EXPECT_NE(text.find("remote_control.default_channel"), std::string::npos);
    EXPECT_EQ(text.find("/rc/send"), std::string::npos);
}

TEST(RemoteControlCommandDisplay, StoppedWithDefaultChannelShowsRcActivationHint) {
    RemoteControlDisplaySnapshot snap;
    snap.running = false;
    snap.port = 28190;
    snap.default_channel = "chat";

    std::string text = format_remote_control_display(snap);
    EXPECT_NE(text.find("Default channel: chat"), std::string::npos);
    EXPECT_NE(text.find("Run /rc to activate the default channel."), std::string::npos);
    EXPECT_NE(text.find("/remote-control on"), std::string::npos);
    EXPECT_EQ(text.find("/rc/send"), std::string::npos);
}

TEST(RemoteControlCommandDisplay, RunningWithActiveChannelShowsChannelState) {
    RemoteControlDisplaySnapshot snap;
    snap.running = true;
    snap.port = 28190;
    snap.token = "abc123";
    snap.default_channel = "chat";
    snap.active_channel = "chat";
    snap.outbound_url = "http://127.0.0.1:9000/hook";

    std::string text = format_remote_control_display(snap);
    EXPECT_NE(text.find("Default channel: chat"), std::string::npos);
    EXPECT_NE(text.find("Active channel : chat"), std::string::npos);
    EXPECT_NE(text.find("http://127.0.0.1:28190/rc/send"), std::string::npos);
}
