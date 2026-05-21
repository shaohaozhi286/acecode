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
