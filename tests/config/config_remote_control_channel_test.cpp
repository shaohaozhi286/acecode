#include <gtest/gtest.h>

#include "config/config.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

using namespace acecode;

TEST(ConfigRemoteControlChannel, DefaultsAreValid) {
    AppConfig cfg;
    EXPECT_TRUE(cfg.remote_control.default_channel.empty());
    EXPECT_TRUE(cfg.remote_control.channels.empty());
    EXPECT_TRUE(validate_config(cfg).empty());
}

TEST(ConfigRemoteControlChannel, DefaultChannelMustExist) {
    AppConfig cfg;
    cfg.remote_control.default_channel = "chat";

    const auto errors = validate_config(cfg);
    bool found = false;
    for (const auto& error : errors) {
        if (error.find("remote_control.default_channel") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST(ConfigRemoteControlChannel, ValidatesChannelShape) {
    AppConfig cfg;
    cfg.remote_control.channels["chat"].timeout_ms = 500;
    cfg.remote_control.channels["bad name"].manifest_path = "C:/plugins/chat/plugin.json";
    cfg.remote_control.channels["bad-settings"].manifest_path =
        "C:/plugins/chat/plugin.json";
    cfg.remote_control.channels["bad-settings"].settings = "not an object";

    const auto errors = validate_config(cfg);
    bool found_missing_manifest = false;
    bool found_bad_timeout = false;
    bool found_bad_name = false;
    bool found_bad_settings = false;
    for (const auto& error : errors) {
        if (error.find(".manifest_path") != std::string::npos) {
            found_missing_manifest = true;
        }
        if (error.find(".timeout_ms") != std::string::npos) {
            found_bad_timeout = true;
        }
        if (error.find("whitespace") != std::string::npos) {
            found_bad_name = true;
        }
        if (error.find(".settings") != std::string::npos) {
            found_bad_settings = true;
        }
    }
    EXPECT_TRUE(found_missing_manifest);
    EXPECT_TRUE(found_bad_timeout);
    EXPECT_TRUE(found_bad_name);
    EXPECT_TRUE(found_bad_settings);
}

TEST(ConfigRemoteControlChannel, PersistsNonDefaultValues) {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
        ("acecode-remote-control-channel-config-test-" + std::to_string(suffix) + ".json");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    AppConfig cfg;
    cfg.remote_control.default_channel = "chat";
    cfg.remote_control.channels["chat"].manifest_path = "C:/plugins/chat/channel-plugin.json";
    cfg.remote_control.channels["chat"].timeout_ms = 15000;
    cfg.remote_control.channels["chat"].settings =
        nlohmann::json{{"profile", "work"}, {"auto_login", true}};
    ASSERT_TRUE(validate_config(cfg).empty());

    save_config(cfg, path.string());

    std::ifstream ifs(path);
    ASSERT_TRUE(ifs.is_open());
    const auto j = nlohmann::json::parse(ifs);
    ASSERT_TRUE(j.contains("remote_control"));
    const auto& rc = j["remote_control"];
    EXPECT_EQ(rc["default_channel"], "chat");
    ASSERT_TRUE(rc.contains("channels"));
    ASSERT_TRUE(rc["channels"].contains("chat"));
    EXPECT_EQ(rc["channels"]["chat"]["manifest_path"],
              "C:/plugins/chat/channel-plugin.json");
    EXPECT_EQ(rc["channels"]["chat"]["timeout_ms"], 15000);
    EXPECT_EQ(rc["channels"]["chat"]["settings"]["profile"], "work");
    EXPECT_EQ(rc["channels"]["chat"]["settings"]["auto_login"], true);

    std::filesystem::remove(path, ec);
}

TEST(ConfigRemoteControlChannel, OmitsDefaultBoundSessionId) {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
        ("acecode-remote-control-bound-default-test-" + std::to_string(suffix) + ".json");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    AppConfig cfg;
    EXPECT_TRUE(cfg.remote_control.bound_session_id.empty());
    save_config(cfg, path.string());

    std::ifstream ifs(path);
    ASSERT_TRUE(ifs.is_open());
    const auto j = nlohmann::json::parse(ifs);
    if (j.contains("remote_control")) {
        EXPECT_FALSE(j["remote_control"].contains("bound_session_id"));
    }

    std::filesystem::remove(path, ec);
}

TEST(ConfigRemoteControlChannel, PersistsBoundSessionId) {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
        ("acecode-remote-control-bound-persist-test-" + std::to_string(suffix) + ".json");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    AppConfig cfg;
    cfg.remote_control.bound_session_id = "ses-daemon-42";
    ASSERT_TRUE(validate_config(cfg).empty());
    save_config(cfg, path.string());

    std::ifstream ifs(path);
    ASSERT_TRUE(ifs.is_open());
    const auto j = nlohmann::json::parse(ifs);
    ASSERT_TRUE(j.contains("remote_control"));
    EXPECT_EQ(j["remote_control"]["bound_session_id"], "ses-daemon-42");

    std::filesystem::remove(path, ec);
}

// 真实 load_config() 解析路径的往返验证(home 覆写到临时目录,与
// config_first_init_test 同款做法):bound_session_id 落盘后重启能读回。
TEST(ConfigRemoteControlChannel, BoundSessionIdRoundTripsThroughLoadConfig) {
#ifdef _WIN32
    constexpr const char* kHomeEnvName = "USERPROFILE";
#else
    constexpr const char* kHomeEnvName = "HOME";
#endif
    const char* previous = std::getenv(kHomeEnvName);
    const std::string previous_home = previous ? previous : "";

    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto temp_home = std::filesystem::temp_directory_path() /
        ("acecode-remote-control-bound-home-" + std::to_string(suffix));
    std::filesystem::create_directories(temp_home);
#ifdef _WIN32
    _putenv_s(kHomeEnvName, temp_home.string().c_str());
#else
    setenv(kHomeEnvName, temp_home.string().c_str(), 1);
#endif
    reset_acecode_home_created_flag_for_test();

    {
        AppConfig cfg;
        cfg.remote_control.bound_session_id = "ses-rebuild-me";
        cfg.remote_control.token = "tok-abc";
        save_config(cfg);

        AppConfig reloaded = load_config();
        EXPECT_EQ(reloaded.remote_control.bound_session_id, "ses-rebuild-me");
        EXPECT_EQ(reloaded.remote_control.token, "tok-abc");
    }

#ifdef _WIN32
    _putenv_s(kHomeEnvName, previous_home.c_str());
#else
    if (previous) setenv(kHomeEnvName, previous_home.c_str(), 1);
    else unsetenv(kHomeEnvName);
#endif
    reset_acecode_home_created_flag_for_test();
    std::error_code ec;
    std::filesystem::remove_all(temp_home, ec);
}
