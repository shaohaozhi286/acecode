#include <gtest/gtest.h>

#include "commands/remote_control_command.hpp"

using acecode::CommandRegistry;
using acecode::RemoteControlDisplaySnapshot;
using acecode::format_remote_control_display;

// 场景:命令注册。期望:/remote-control 与短别名 /rc 同时可用 —— 别名是
// 对外承诺的入口(README 已写明),重构注册逻辑时不能丢。
TEST(RemoteControlCommand, RegistersBothFullNameAndRcAlias) {
    CommandRegistry registry;
    acecode::register_remote_control_command(registry);
    EXPECT_TRUE(registry.has_command("remote-control"));
    EXPECT_TRUE(registry.has_command("rc"));
}

// 场景:/remote-control 状态输出(运行中)。期望:包含入站端点、token header、
// 出站 webhook 与统计行 —— 这是用户给 IM 桥做配对时照抄的全部信息。
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
    EXPECT_EQ(text.find("/rc/send"), std::string::npos);
}
