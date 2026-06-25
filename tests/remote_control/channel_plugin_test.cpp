#include <gtest/gtest.h>

#include "remote_control/channel_plugin.hpp"

#include <nlohmann/json.hpp>

#include <string>

using acecode::HookCommandSpec;
using acecode::HookProcessResult;
using acecode::rc::ChannelActivationRequest;
using acecode::rc::ChannelPluginHost;
using acecode::rc::ChannelPluginManifest;
using acecode::rc::ChannelPluginStatus;

TEST(ChannelPluginManifest, ParsesStdioLauncher) {
    const auto j = nlohmann::json{
        {"schema", "acecode.channel-plugin.v1"},
        {"name", "chat"},
        {"transport", "stdio"},
        {"launcher",
         nlohmann::json{
             {"command", "chat-channel.exe"},
             {"args", nlohmann::json::array({"--profile", "work"})},
             {"cwd", "C:/plugins/chat"},
         }},
        {"timeout_ms", 15000},
    };

    ChannelPluginManifest manifest;
    std::string error;
    ASSERT_TRUE(acecode::rc::parse_channel_plugin_manifest_json(j, &manifest, &error))
        << error;
    EXPECT_EQ(manifest.name, "chat");
    EXPECT_EQ(manifest.transport, "stdio");
    EXPECT_EQ(manifest.command, "chat-channel.exe");
    ASSERT_EQ(manifest.args.size(), 2u);
    EXPECT_EQ(manifest.args[0], "--profile");
    EXPECT_EQ(manifest.args[1], "work");
    EXPECT_EQ(manifest.cwd, "C:/plugins/chat");
    EXPECT_EQ(manifest.timeout_ms, 15000);
}

TEST(ChannelPluginManifest, RejectsUnsupportedManifest) {
    const auto j = nlohmann::json{
        {"schema", "acecode.channel-plugin.v9"},
        {"name", "chat"},
        {"transport", "stdio"},
        {"command", "chat-channel.exe"},
    };

    ChannelPluginManifest manifest;
    std::string error;
    EXPECT_FALSE(acecode::rc::parse_channel_plugin_manifest_json(j, &manifest, &error));
    EXPECT_NE(error.find("unsupported"), std::string::npos);
}

TEST(ChannelActivationRequest, BuildsNeutralPayload) {
    ChannelActivationRequest request;
    request.session_id = "session-1";
    request.inbound_url = "http://127.0.0.1:28190/rc/send";
    request.token = "tok";
    request.settings = nlohmann::json{{"profile", "work"}};

    const auto j = acecode::rc::channel_activation_request_to_json(request);
    EXPECT_EQ(j["type"], "channel.activate");
    EXPECT_EQ(j["protocol_version"], 1);
    EXPECT_EQ(j["session_id"], "session-1");
    EXPECT_EQ(j["inbound"]["url"], "http://127.0.0.1:28190/rc/send");
    EXPECT_EQ(j["inbound"]["token_header"], "X-ACECode-RC-Token");
    EXPECT_EQ(j["inbound"]["token"], "tok");
    EXPECT_EQ(j["outbound"]["preferred"], "webhook");
    EXPECT_EQ(j["settings"]["profile"], "work");
}

TEST(ChannelPluginStatus, ParsesConnectedWebhookStatus) {
    const auto j = nlohmann::json{
        {"type", "channel.status"},
        {"state", "connected"},
        {"already_running", true},
        {"outbound",
         nlohmann::json{
             {"mode", "webhook"},
             {"url", "http://127.0.0.1:39001/messages"},
         }},
    };

    ChannelPluginStatus status;
    std::string error;
    ASSERT_TRUE(acecode::rc::parse_channel_plugin_status_json(j, &status, &error))
        << error;
    EXPECT_TRUE(status.connected());
    EXPECT_TRUE(status.already_running);
    EXPECT_EQ(status.outbound_mode, "webhook");
    EXPECT_EQ(status.outbound_url, "http://127.0.0.1:39001/messages");
}

TEST(ChannelPluginHost, ActivatesAndAcceptsAlreadyRunningRuntime) {
    ChannelPluginManifest manifest;
    manifest.name = "chat";
    manifest.command = "chat-channel.exe";
    manifest.args = {"--stdio"};
    manifest.cwd = "C:/plugins/chat";

    std::string seen_stdin;
    HookCommandSpec seen_command;
    int seen_timeout = 0;
    std::string seen_cwd;
    ChannelPluginHost host([&](const HookCommandSpec& command,
                               const std::string& stdin_text,
                               int timeout_ms,
                               const std::string& cwd) {
        seen_command = command;
        seen_stdin = stdin_text;
        seen_timeout = timeout_ms;
        seen_cwd = cwd;
        HookProcessResult result;
        result.started = true;
        result.exit_code = 0;
        result.stdout_text =
            R"({"type":"channel.status","state":"connected","already_running":true,"outbound":{"mode":"webhook","url":"http://127.0.0.1:39001/messages"}})";
        return result;
    });

    ChannelActivationRequest request;
    request.session_id = "session-1";
    request.inbound_url = "http://127.0.0.1:28190/rc/send";
    request.token = "tok";
    request.settings = nlohmann::json{{"profile", "work"}};

    std::string error;
    const auto activation = host.activate(manifest, request, 12345, &error);
    ASSERT_TRUE(activation.ok) << error;
    EXPECT_TRUE(activation.status.already_running);
    EXPECT_EQ(activation.status.outbound_url, "http://127.0.0.1:39001/messages");
    EXPECT_EQ(seen_command.command, "chat-channel.exe");
    ASSERT_EQ(seen_command.args.size(), 1u);
    EXPECT_EQ(seen_command.args[0], "--stdio");
    EXPECT_EQ(seen_timeout, 12345);
    EXPECT_EQ(seen_cwd, "C:/plugins/chat");

    const auto sent = nlohmann::json::parse(seen_stdin);
    EXPECT_EQ(sent["type"], "channel.activate");
    EXPECT_EQ(sent["session_id"], "session-1");
    EXPECT_EQ(sent["settings"]["profile"], "work");
}

TEST(ChannelPluginHost, ReportsPluginFailure) {
    ChannelPluginManifest manifest;
    manifest.name = "chat";
    manifest.command = "chat-channel.exe";

    ChannelPluginHost host([](const HookCommandSpec&,
                              const std::string&,
                              int,
                              const std::string&) {
        HookProcessResult result;
        result.started = true;
        result.exit_code = 0;
        result.stdout_text =
            R"({"type":"channel.status","state":"failed","message":"login required"})";
        return result;
    });

    ChannelActivationRequest request;
    request.session_id = "session-1";
    request.inbound_url = "http://127.0.0.1:28190/rc/send";
    request.token = "tok";

    std::string error;
    const auto activation = host.activate(manifest, request, 10000, &error);
    EXPECT_FALSE(activation.ok);
    EXPECT_NE(error.find("login required"), std::string::npos);
}
