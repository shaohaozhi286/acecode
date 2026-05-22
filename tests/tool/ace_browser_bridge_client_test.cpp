#include <gtest/gtest.h>

#include "tool/ace_browser_bridge/client.hpp"

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using namespace acecode;
using namespace acecode::ace_browser_bridge;

namespace {

struct Call {
    std::vector<std::string> argv;
    std::string stdin_text;
};

} // namespace

TEST(AceBrowserBridgeClientEnvelope, ParsesSuccessEnvelope) {
    auto envelope = AceBrowserBridgeClient::parse_envelope(R"({"ok":true,"data":{"running":true}})");
    ASSERT_TRUE(envelope.ok);
    EXPECT_TRUE(envelope.data["running"].get<bool>());
    EXPECT_FALSE(envelope.error.has_value());
}

TEST(AceBrowserBridgeClientEnvelope, ParsesFailureEnvelope) {
    auto envelope = AceBrowserBridgeClient::parse_envelope(
        R"({"ok":false,"error":{"code":"extension_not_connected","message":"no extension connected"}})");
    ASSERT_FALSE(envelope.ok);
    ASSERT_TRUE(envelope.error.has_value());
    EXPECT_EQ(envelope.error->code, "extension_not_connected");
    EXPECT_EQ(envelope.error->message, "no extension connected");
}

TEST(AceBrowserBridgeClientEnvelope, RejectsInvalidEnvelope) {
    auto envelope = AceBrowserBridgeClient::parse_envelope("not-json");
    ASSERT_FALSE(envelope.ok);
    ASSERT_TRUE(envelope.error.has_value());
    EXPECT_EQ(envelope.error->code, "invalid_host_response");
}

TEST(AceBrowserBridgeClientInvocation, BuildsCommandArgvAndStdin) {
    auto calls = std::make_shared<std::vector<Call>>();
    AceBrowserBridgeConfig cfg;
    cfg.host_path = "custom-host";
    AceBrowserBridgeClient client(cfg, [calls](const std::vector<std::string>& argv,
                                               const std::string& stdin_text,
                                               std::chrono::milliseconds) {
        calls->push_back(Call{argv, stdin_text});
        return CliProcessResult{0, false, R"({"ok":true,"data":{"success":true}})", ""};
    });

    BrowserCommandRequest req;
    req.session = "demo";
    req.action = "snapshot";
    req.args = {{"mode", "summary"}};
    auto envelope = client.command(req);

    ASSERT_TRUE(envelope.ok);
    ASSERT_EQ(calls->size(), 1u);
    EXPECT_EQ((*calls)[0].argv, (std::vector<std::string>{"custom-host", "command", "--json"}));
    auto sent = nlohmann::json::parse((*calls)[0].stdin_text);
    EXPECT_EQ(sent["session"], "demo");
    EXPECT_EQ(sent["action"], "snapshot");
    EXPECT_EQ(sent["args"]["mode"], "summary");
}

TEST(AceBrowserBridgeClientInvocation, BuildsScreenshotArgv) {
    auto calls = std::make_shared<std::vector<Call>>();
    AceBrowserBridgeConfig cfg;
    cfg.host_path = "custom-host";
    AceBrowserBridgeClient client(cfg, [calls](const std::vector<std::string>& argv,
                                               const std::string& stdin_text,
                                               std::chrono::milliseconds) {
        calls->push_back(Call{argv, stdin_text});
        return CliProcessResult{0, false, R"({"ok":true,"data":{"path":"C:/tmp/page.png"}})", ""};
    });

    auto envelope = client.screenshot("demo", "C:/tmp/page.png");

    ASSERT_TRUE(envelope.ok);
    ASSERT_EQ(calls->size(), 1u);
    EXPECT_EQ((*calls)[0].argv,
              (std::vector<std::string>{"custom-host", "screenshot", "--json",
                                        "--session", "demo", "--output", "C:/tmp/page.png"}));
    EXPECT_EQ((*calls)[0].stdin_text, "");
}

TEST(AceBrowserBridgeClientStatus, CachesOnlyHealthyStatus) {
    auto calls = std::make_shared<int>(0);
    AceBrowserBridgeConfig cfg;
    cfg.status_cache_ttl_ms = 10000;
    AceBrowserBridgeClient client(cfg, [calls](const std::vector<std::string>&,
                                               const std::string&,
                                               std::chrono::milliseconds) {
        ++(*calls);
        return CliProcessResult{0, false,
            R"({"ok":true,"data":{"running":true,"extension_connected":true}})", ""};
    });

    EXPECT_TRUE(client.status().ok);
    EXPECT_TRUE(client.status().ok);
    EXPECT_EQ(*calls, 1);
}

TEST(AceBrowserBridgeClientStatus, DoesNotCacheUnhealthyStatus) {
    auto calls = std::make_shared<int>(0);
    AceBrowserBridgeConfig cfg;
    cfg.status_cache_ttl_ms = 10000;
    AceBrowserBridgeClient client(cfg, [calls](const std::vector<std::string>&,
                                               const std::string&,
                                               std::chrono::milliseconds) {
        ++(*calls);
        return CliProcessResult{0, false,
            R"({"ok":true,"data":{"running":true,"extension_connected":false}})", ""};
    });

    EXPECT_TRUE(client.status().ok);
    EXPECT_TRUE(client.status().ok);
    EXPECT_EQ(*calls, 2);
}

TEST(AceBrowserBridgeClientStatus, AutoStartsHostWhenStatusReportsStopped) {
    auto calls = std::make_shared<std::vector<Call>>();
    auto started = std::make_shared<int>(0);
    AceBrowserBridgeConfig cfg;
    cfg.host_path = "custom-host";
    AceBrowserBridgeClient client(
        cfg,
        [calls, started](const std::vector<std::string>& argv,
                         const std::string& stdin_text,
                         std::chrono::milliseconds) {
            calls->push_back(Call{argv, stdin_text});
            if (*started == 0) {
                return CliProcessResult{0, false,
                    R"({"ok":true,"data":{"running":false,"extension_connected":false}})", ""};
            }
            return CliProcessResult{0, false,
                R"({"ok":true,"data":{"running":true,"extension_connected":false}})", ""};
        },
        [started](const std::vector<std::string>& argv) {
            EXPECT_EQ(argv, (std::vector<std::string>{"custom-host", "serve", "--json", "--port", "52007"}));
            ++(*started);
            return HostStartResult{true, ""};
        });

    auto envelope = client.status();

    ASSERT_TRUE(envelope.ok);
    EXPECT_TRUE(envelope.data["running"].get<bool>());
    EXPECT_FALSE(envelope.data["extension_connected"].get<bool>());
    EXPECT_TRUE(envelope.data["auto_start_attempted"].get<bool>());
    EXPECT_TRUE(envelope.data["auto_started"].get<bool>());
    EXPECT_EQ(*started, 1);
    ASSERT_GE(calls->size(), 2u);
}

TEST(AceBrowserBridgeClientStatus, ReportsAutoStartFailureInStatusData) {
    auto started = std::make_shared<int>(0);
    AceBrowserBridgeConfig cfg;
    cfg.host_path = "custom-host";
    AceBrowserBridgeClient client(
        cfg,
        [](const std::vector<std::string>&,
           const std::string&,
           std::chrono::milliseconds) {
            return CliProcessResult{0, false,
                R"({"ok":true,"data":{"running":false,"extension_connected":false}})", ""};
        },
        [started](const std::vector<std::string>&) {
            ++(*started);
            return HostStartResult{false, "boom"};
        });

    auto envelope = client.status();

    ASSERT_TRUE(envelope.ok);
    EXPECT_FALSE(envelope.data["running"].get<bool>());
    EXPECT_TRUE(envelope.data["auto_start_attempted"].get<bool>());
    EXPECT_EQ(envelope.data["auto_start_error"], "boom");
    EXPECT_EQ(*started, 1);
}

TEST(AceBrowserBridgeClientInvocation, RetriesCommandAfterStartingStoppedHost) {
    auto calls = std::make_shared<std::vector<Call>>();
    auto started = std::make_shared<int>(0);
    auto command_calls = std::make_shared<int>(0);
    AceBrowserBridgeConfig cfg;
    cfg.host_path = "custom-host";
    AceBrowserBridgeClient client(
        cfg,
        [calls, started, command_calls](const std::vector<std::string>& argv,
                                        const std::string& stdin_text,
                                        std::chrono::milliseconds) {
            calls->push_back(Call{argv, stdin_text});
            if (argv.size() >= 2 && argv[1] == "command") {
                ++(*command_calls);
                if (*command_calls == 1) {
                    return CliProcessResult{0, false,
                        R"({"ok":false,"error":{"code":"daemon_not_running","message":"not running"}})", ""};
                }
                return CliProcessResult{0, false,
                    R"({"ok":true,"data":{"success":true}})", ""};
            }
            if (*started == 0) {
                return CliProcessResult{0, false,
                    R"({"ok":true,"data":{"running":false,"extension_connected":false}})", ""};
            }
            return CliProcessResult{0, false,
                R"({"ok":true,"data":{"running":true,"extension_connected":true}})", ""};
        },
        [started](const std::vector<std::string>&) {
            ++(*started);
            return HostStartResult{true, ""};
        });

    BrowserCommandRequest req;
    req.session = "demo";
    req.action = "snapshot";
    auto envelope = client.command(req);

    ASSERT_TRUE(envelope.ok);
    EXPECT_EQ(envelope.data["success"], true);
    EXPECT_EQ(*started, 1);
    EXPECT_EQ(*command_calls, 2);
    ASSERT_GE(calls->size(), 4u);
}

TEST(AceBrowserBridgeClientInvocation, MapsTimeout) {
    AceBrowserBridgeClient client(AceBrowserBridgeConfig{},
        [](const std::vector<std::string>&, const std::string&, std::chrono::milliseconds) {
            return CliProcessResult{-1, true, "", ""};
        });

    auto envelope = client.status();
    ASSERT_FALSE(envelope.ok);
    ASSERT_TRUE(envelope.error.has_value());
    EXPECT_EQ(envelope.error->code, "host_timeout");
}

TEST(AceBrowserBridgeClientInvocation, MapsProcessLaunchErrorToCliNotFound) {
    AceBrowserBridgeClient client(AceBrowserBridgeConfig{},
        [](const std::vector<std::string>&, const std::string&, std::chrono::milliseconds) {
            return CliProcessResult{-1, false, "", "Failed to start ace-browser-host"};
        });

    auto envelope = client.status();
    ASSERT_FALSE(envelope.ok);
    ASSERT_TRUE(envelope.error.has_value());
    EXPECT_EQ(envelope.error->code, "host_not_found");
}
