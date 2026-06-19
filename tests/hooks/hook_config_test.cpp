#include <gtest/gtest.h>

#include "hooks/hook_config.hpp"
#include "utils/utf8_path.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

using acecode::HookMode;
using acecode::HookPlatform;

namespace {

struct TempHookConfigFile {
    std::filesystem::path root;
    std::filesystem::path path;

    TempHookConfigFile() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root = std::filesystem::temp_directory_path() /
               ("acecode-hook-config-test-" + std::to_string(stamp));
        path = root / "hooks.json";
        std::filesystem::create_directories(root);
    }

    ~TempHookConfigFile() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
};

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    ofs << text;
}

} // namespace

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

TEST(HookConfig, WritesEnabledFlagPreservingLegacyEvents) {
    TempHookConfigFile tmp;
    write_text(tmp.path, R"({
        "version": 1,
        "enabled": true,
        "events": {
            "startup.before_model_load": [
                {"id": "startup", "command": "node", "args": ["hook.js"]}
            ]
        }
    })");

    std::string error;
    const std::string path = acecode::path_to_utf8(tmp.path);
    ASSERT_TRUE(acecode::set_hook_config_enabled_in_path(path, false, &error)) << error;

    auto disabled = acecode::load_hook_config_from_path(path, &error);
    EXPECT_TRUE(error.empty());
    EXPECT_FALSE(disabled.enabled);
    ASSERT_EQ(disabled.events["startup.before_model_load"].size(), 1u);
    EXPECT_EQ(disabled.events["startup.before_model_load"][0].command.command, "node");

    ASSERT_TRUE(acecode::set_hook_config_enabled_in_path(path, true, &error)) << error;
    auto enabled = acecode::load_hook_config_from_path(path, &error);
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(enabled.enabled);
    ASSERT_EQ(enabled.events["startup.before_model_load"].size(), 1u);
}
