#include <gtest/gtest.h>

#include "web/handlers/builtin_command_handler.hpp"

TEST(BuiltinCommandHandler, ParsesSupportedCommandName) {
    auto parsed = acecode::web::parse_builtin_command_request(
        R"({"command":"init","display_text":"/init"})");

    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(parsed.request.name, "init");
    EXPECT_EQ(parsed.request.args, "");
    EXPECT_EQ(parsed.request.display_text, "/init");
}

TEST(BuiltinCommandHandler, ParsesSlashTextAndArgs) {
    auto parsed = acecode::web::parse_builtin_command_request(
        R"({"command":"/compact now"})");

    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(parsed.request.name, "compact");
    EXPECT_EQ(parsed.request.args, "now");
    EXPECT_EQ(parsed.request.display_text, "/compact now");
}

TEST(BuiltinCommandHandler, ParsesGoalCommand) {
    auto parsed = acecode::web::parse_builtin_command_request(
        R"({"command":"/goal --tokens 50K finish migration"})");

    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(parsed.request.name, "goal");
    EXPECT_EQ(parsed.request.args, "--tokens 50K finish migration");
    EXPECT_EQ(parsed.request.display_text, "/goal --tokens 50K finish migration");
}

TEST(BuiltinCommandHandler, ParsesPlanCommand) {
    auto parsed = acecode::web::parse_builtin_command_request(
        R"({"command":"/plan inspect the updater flow"})");

    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(parsed.request.name, "plan");
    EXPECT_EQ(parsed.request.args, "inspect the updater flow");
    EXPECT_EQ(parsed.request.display_text, "/plan inspect the updater flow");
}

TEST(BuiltinCommandHandler, RejectsUnsupportedCommand) {
    auto parsed = acecode::web::parse_builtin_command_request(
        R"({"command":"model"})");

    EXPECT_FALSE(parsed.ok);
    EXPECT_EQ(parsed.status, 400);
    EXPECT_EQ(parsed.error, "unsupported command");
    EXPECT_EQ(parsed.request.name, "model");
    auto body = acecode::web::builtin_command_error_json(parsed);
    EXPECT_EQ(body["error"], "unsupported command");
    EXPECT_EQ(body["command"], "model");
}

TEST(BuiltinCommandHandler, RejectsBadJson) {
    auto parsed = acecode::web::parse_builtin_command_request("{");

    EXPECT_FALSE(parsed.ok);
    EXPECT_EQ(parsed.status, 400);
    EXPECT_NE(parsed.error.find("bad json"), std::string::npos);
}

// daemon 托管 remote control:/rc 与 /remote-control 必须能通过 HTTP 命令
// 网关(实际执行由 SessionRegistry 的 external command handler 承接)。
TEST(BuiltinCommandHandler, ParsesRemoteControlCommands) {
    auto rc = acecode::web::parse_builtin_command_request(
        R"({"command":"/rc"})");
    ASSERT_TRUE(rc.ok) << rc.error;
    EXPECT_EQ(rc.request.name, "rc");
    EXPECT_EQ(rc.request.args, "");

    auto rc_off = acecode::web::parse_builtin_command_request(
        R"({"command":"/rc off"})");
    ASSERT_TRUE(rc_off.ok) << rc_off.error;
    EXPECT_EQ(rc_off.request.name, "rc");
    EXPECT_EQ(rc_off.request.args, "off");

    auto full = acecode::web::parse_builtin_command_request(
        R"({"command":"/remote-control show"})");
    ASSERT_TRUE(full.ok) << full.error;
    EXPECT_EQ(full.request.name, "remote-control");
    EXPECT_EQ(full.request.args, "show");
}
