#include <gtest/gtest.h>

#include "hooks/hook_config.hpp"

#include <nlohmann/json.hpp>

using acecode::HookMode;
using acecode::HookPlatform;

TEST(HookConfig, ParsesEventsAndPlatformCommands) {
    auto j = nlohmann::json::parse(R"({
        "version": 1,
        "enabled": true,
        "events": {
            "assistant.message_completed": [
                {
                    "id": "assistant-done",
                    "mode": "async",
                    "platforms": ["posix"],
                    "commands": {
                        "posix": {"command": "python3", "args": ["hook.py"]},
                        "linux": {"command": "python3", "args": ["linux.py"]},
                        "windows": {"command": "python", "args": ["hook.py"]}
                    },
                    "timeout_ms": 0
                }
            ]
        }
    })");

    std::string error;
    auto cfg = acecode::parse_hook_config_json(j, &error);
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(cfg.enabled);
    ASSERT_EQ(cfg.events["assistant.message_completed"].size(), 1u);

    const auto& hook = cfg.events["assistant.message_completed"][0];
    EXPECT_EQ(hook.id, "assistant-done");
    EXPECT_EQ(hook.mode, HookMode::Async);
    EXPECT_EQ(hook.timeout_ms, 0);
    EXPECT_TRUE(acecode::hook_platform_matches(hook, HookPlatform::Linux));
    EXPECT_TRUE(acecode::hook_platform_matches(hook, HookPlatform::Mac));
    EXPECT_FALSE(acecode::hook_platform_matches(hook, HookPlatform::Windows));

    auto linux_cmd = acecode::resolve_hook_command(hook, HookPlatform::Linux);
    ASSERT_TRUE(linux_cmd.has_value());
    EXPECT_EQ(linux_cmd->args[0], "linux.py");

    auto mac_cmd = acecode::resolve_hook_command(hook, HookPlatform::Mac);
    ASSERT_TRUE(mac_cmd.has_value());
    EXPECT_EQ(mac_cmd->args[0], "hook.py");
}

TEST(HookConfig, SpecificPlatformBeatsUnixAndPosix) {
    auto j = nlohmann::json::parse(R"({
        "enabled": true,
        "events": {
            "startup.models_loaded": [
                {
                    "id": "startup",
                    "commands": {
                        "posix": {"command": "sh", "args": ["posix.sh"]},
                        "unix": {"command": "sh", "args": ["unix.sh"]},
                        "mac": {"command": "sh", "args": ["mac.sh"]}
                    }
                }
            ]
        }
    })");

    auto cfg = acecode::parse_hook_config_json(j);
    const auto& hook = cfg.events["startup.models_loaded"][0];

    auto mac_cmd = acecode::resolve_hook_command(hook, HookPlatform::Mac);
    ASSERT_TRUE(mac_cmd.has_value());
    EXPECT_EQ(mac_cmd->args[0], "mac.sh");

    auto linux_cmd = acecode::resolve_hook_command(hook, HookPlatform::Linux);
    ASSERT_TRUE(linux_cmd.has_value());
    EXPECT_EQ(linux_cmd->args[0], "unix.sh");
}

