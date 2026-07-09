#include "config/config.hpp"
#include "utils/paths.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

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
constexpr const char* kOpenAiStreamTimeoutEnvName = "ACECODE_OPENAI_STREAM_TIMEOUT_MS";

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

fs::path make_temp_root() {
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path root = fs::temp_directory_path() /
        ("acecode-config-first-init-" + std::to_string(now));
    std::error_code ec;
    fs::remove_all(root, ec);
    return root;
}

class ConfigFirstInitTest : public ::testing::Test {
protected:
    fs::path temp_home;
    std::string previous_home;
    std::string previous_stream_timeout;
    bool had_previous_home = false;
    bool had_previous_stream_timeout = false;

    void SetUp() override {
        acecode::reset_run_mode_for_test();
        acecode::reset_acecode_home_created_flag_for_test();
        if (const char* existing = std::getenv(kHomeEnvName)) {
            previous_home = existing;
            had_previous_home = true;
        }
        if (const char* existing = std::getenv(kOpenAiStreamTimeoutEnvName)) {
            previous_stream_timeout = existing;
            had_previous_stream_timeout = true;
        }
        temp_home = make_temp_root();
        set_env_value(kHomeEnvName, temp_home.string());
        clear_env_value(kOpenAiStreamTimeoutEnvName);
    }

    void TearDown() override {
        if (had_previous_home) {
            set_env_value(kHomeEnvName, previous_home);
        } else {
            clear_env_value(kHomeEnvName);
        }
        if (had_previous_stream_timeout) {
            set_env_value(kOpenAiStreamTimeoutEnvName, previous_stream_timeout);
        } else {
            clear_env_value(kOpenAiStreamTimeoutEnvName);
        }
        acecode::reset_acecode_home_created_flag_for_test();
        acecode::reset_run_mode_for_test();
        std::error_code ec;
        fs::remove_all(temp_home, ec);
    }
};

TEST_F(ConfigFirstInitTest, LoadConfigTracksFreshAcecodeHomeCreationOnce) {
    ASSERT_FALSE(fs::exists(temp_home / ".acecode"));

    auto cfg = acecode::load_config();

    EXPECT_TRUE(fs::is_directory(temp_home / ".acecode"));
    EXPECT_TRUE(acecode::was_acecode_home_created_by_process());
    EXPECT_TRUE(acecode::consume_acecode_home_created_by_process());
    EXPECT_FALSE(acecode::was_acecode_home_created_by_process());

    (void)acecode::load_config();

    EXPECT_FALSE(acecode::consume_acecode_home_created_by_process());

    EXPECT_TRUE(cfg.provider.empty());
    EXPECT_TRUE(cfg.saved_models.empty());
    EXPECT_TRUE(cfg.default_model_name.empty());
    EXPECT_EQ(cfg.default_permission_mode, "default");

    std::ifstream ifs(temp_home / ".acecode" / "config.json");
    ASSERT_TRUE(ifs.is_open());
    auto j = nlohmann::json::parse(ifs);
    ASSERT_TRUE(j.contains("saved_models"));
    ASSERT_TRUE(j["saved_models"].is_array());
    EXPECT_TRUE(j["saved_models"].empty());
    EXPECT_EQ(j["provider"], "");
    EXPECT_EQ(j["default_model_name"], "");
    EXPECT_EQ(j["default_permission_mode"], "default");
}

TEST_F(ConfigFirstInitTest, LegacyTlsPolicyKeysAreIgnoredAndNotPersisted) {
    fs::create_directories(temp_home / ".acecode");
    const fs::path config_path = temp_home / ".acecode" / "config.json";
    {
        std::ofstream ofs(config_path);
        ofs << R"({
    "network": {
        "proxy_mode": "off",
        "proxy_ca_bundle": "C:\\tmp\\capture.pem",
        "proxy_insecure_skip_verify": true
    },
    "mcp_servers": {
        "remote": {
            "transport": "sse",
            "url": "https://mcp.example.com",
            "validate_certificates": false,
            "ca_cert_path": "C:\\tmp\\mcp-ca.pem",
            "timeout_seconds": 12
        }
    }
})";
    }

    auto cfg = acecode::load_config();
    EXPECT_EQ(cfg.network.proxy_mode, "off");
    ASSERT_EQ(cfg.mcp_servers.size(), 1u);
    EXPECT_EQ(cfg.mcp_servers["remote"].url, "https://mcp.example.com");
    EXPECT_EQ(cfg.mcp_servers["remote"].timeout_seconds, 12);

    acecode::save_config(cfg, config_path.string());

    std::ifstream ifs(config_path);
    ASSERT_TRUE(ifs.is_open());
    auto saved = nlohmann::json::parse(ifs);
    ASSERT_TRUE(saved["network"].is_object());
    EXPECT_FALSE(saved["network"].contains("proxy_ca_bundle"));
    EXPECT_FALSE(saved["network"].contains("proxy_insecure_skip_verify"));
    ASSERT_TRUE(saved["mcp_servers"]["remote"].is_object());
    EXPECT_FALSE(saved["mcp_servers"]["remote"].contains("validate_certificates"));
    EXPECT_FALSE(saved["mcp_servers"]["remote"].contains("ca_cert_path"));
}

// 触发场景:一个 mcp server 配了 disabled:true,另一个没配(启用)。
// 期望:load 把 disabled 读进 McpServerConfig;save 只对禁用的 server 写出
// disabled 键(启用态保持稀疏,与其它布尔字段一致),重载后状态不丢。
// 回归:设置页开关落盘依赖这条 round-trip,漏写字段会让重启后开关状态还原。
TEST_F(ConfigFirstInitTest, McpDisabledFlagRoundTripsAndStaysSparse) {
    fs::create_directories(temp_home / ".acecode");
    const fs::path config_path = temp_home / ".acecode" / "config.json";
    {
        std::ofstream ofs(config_path);
        ofs << R"({
    "mcp_servers": {
        "off_server": {
            "command": "npx",
            "args": ["-y", "@mcp/server-fs", "/path"],
            "disabled": true
        },
        "on_server": {
            "transport": "http",
            "url": "https://mcp.example.com"
        }
    }
})";
    }

    auto cfg = acecode::load_config();
    ASSERT_EQ(cfg.mcp_servers.size(), 2u);
    EXPECT_TRUE(cfg.mcp_servers["off_server"].disabled);
    EXPECT_FALSE(cfg.mcp_servers["on_server"].disabled);

    acecode::save_config(cfg, config_path.string());

    std::ifstream ifs(config_path);
    ASSERT_TRUE(ifs.is_open());
    auto saved = nlohmann::json::parse(ifs);
    ASSERT_TRUE(saved["mcp_servers"].is_object());
    // 禁用 server 写出 disabled:true;启用 server 不带该键。
    EXPECT_TRUE(saved["mcp_servers"]["off_server"].value("disabled", false));
    EXPECT_FALSE(saved["mcp_servers"]["on_server"].contains("disabled"));

    // 重新加载后状态保持一致。
    auto reloaded = acecode::load_config();
    EXPECT_TRUE(reloaded.mcp_servers["off_server"].disabled);
    EXPECT_FALSE(reloaded.mcp_servers["on_server"].disabled);
}

TEST_F(ConfigFirstInitTest, ExplicitEmptySavedModelsDoesNotSynthesizeCopilot) {
    fs::create_directories(temp_home / ".acecode");
    {
        std::ofstream ofs(temp_home / ".acecode" / "config.json");
        ofs << R"({
    "provider": "copilot",
    "copilot": { "model": "gpt-4o" },
    "saved_models": [],
    "default_model_name": "copilot"
})";
    }

    auto cfg = acecode::load_config();

    EXPECT_EQ(cfg.provider, "copilot");
    EXPECT_TRUE(cfg.saved_models.empty());
    EXPECT_TRUE(cfg.default_model_name.empty());
}

TEST_F(ConfigFirstInitTest, OldSchemaConfigSynthesizesCopilotSavedModel) {
    fs::create_directories(temp_home / ".acecode");
    {
        std::ofstream ofs(temp_home / ".acecode" / "config.json");
        ofs << R"({
    "provider": "copilot",
    "copilot": { "model": "gpt-4o" },
    "openai": {
        "base_url": "http://localhost:1234/v1",
        "api_key": "",
        "model": "local-model"
    }
})";
    }

    auto cfg = acecode::load_config();

    ASSERT_EQ(cfg.saved_models.size(), 1u);
    EXPECT_EQ(cfg.saved_models[0].name, "copilot");
    EXPECT_EQ(cfg.saved_models[0].provider, "copilot");
    EXPECT_EQ(cfg.saved_models[0].model, "gpt-4o");
    EXPECT_EQ(cfg.default_model_name, "copilot");
}

TEST_F(ConfigFirstInitTest, OpenAiStreamTimeoutParsesIntoLegacySavedModel) {
    fs::create_directories(temp_home / ".acecode");
    {
        std::ofstream ofs(temp_home / ".acecode" / "config.json");
        ofs << R"({
    "provider": "openai",
    "openai": {
        "base_url": "http://localhost:1234/v1",
        "api_key": "sk-test",
        "model": "local-model",
        "stream_timeout_ms": 240000
    }
})";
    }

    auto cfg = acecode::load_config();

    EXPECT_EQ(cfg.openai.stream_timeout_ms, 240000);
    ASSERT_EQ(cfg.saved_models.size(), 1u);
    ASSERT_TRUE(cfg.saved_models[0].stream_timeout_ms.has_value());
    EXPECT_EQ(*cfg.saved_models[0].stream_timeout_ms, 240000);
}

TEST_F(ConfigFirstInitTest, OpenAiStreamTimeoutEnvOverrideFeedsLegacySavedModel) {
    set_env_value(kOpenAiStreamTimeoutEnvName, "345678");
    fs::create_directories(temp_home / ".acecode");
    {
        std::ofstream ofs(temp_home / ".acecode" / "config.json");
        ofs << R"({
    "provider": "openai",
    "openai": {
        "base_url": "http://localhost:1234/v1",
        "api_key": "sk-test",
        "model": "local-model"
    }
})";
    }

    auto cfg = acecode::load_config();

    EXPECT_EQ(cfg.openai.stream_timeout_ms, 345678);
    ASSERT_EQ(cfg.saved_models.size(), 1u);
    ASSERT_TRUE(cfg.saved_models[0].stream_timeout_ms.has_value());
    EXPECT_EQ(*cfg.saved_models[0].stream_timeout_ms, 345678);
}

TEST_F(ConfigFirstInitTest, OldSchemaCodexConfigDoesNotSynthesizeCopilot) {
    fs::create_directories(temp_home / ".acecode");
    {
        std::ofstream ofs(temp_home / ".acecode" / "config.json");
        ofs << R"({
    "provider": "codex",
    "codex": { "model": "gpt-5.5" },
    "copilot": { "model": "gpt-4o" },
    "openai": {
        "base_url": "http://localhost:1234/v1",
        "api_key": "",
        "model": "local-model"
    }
})";
    }

    auto cfg = acecode::load_config();

    EXPECT_TRUE(cfg.provider.empty());
    EXPECT_TRUE(cfg.saved_models.empty());
    EXPECT_TRUE(cfg.default_model_name.empty());
}

TEST_F(ConfigFirstInitTest, SaveConfigPersistsExplicitEmptySavedModels) {
    acecode::AppConfig cfg;
    cfg.provider = "";
    cfg.saved_models.clear();
    cfg.default_model_name.clear();
    cfg.default_permission_mode = "accept-edits";

    const fs::path config_path = temp_home / ".acecode" / "config.json";
    acecode::save_config(cfg, config_path.string());

    std::ifstream ifs(config_path);
    ASSERT_TRUE(ifs.is_open());
    auto j = nlohmann::json::parse(ifs);
    ASSERT_TRUE(j.contains("saved_models"));
    ASSERT_TRUE(j["saved_models"].is_array());
    EXPECT_TRUE(j["saved_models"].empty());
    EXPECT_FALSE(j.contains("default_model_name"));
    EXPECT_EQ(j["default_permission_mode"], "accept-edits");
}

TEST_F(ConfigFirstInitTest, HooksFeatureFlagDefaultsEnabled) {
    fs::create_directories(temp_home / ".acecode");
    {
        std::ofstream ofs(temp_home / ".acecode" / "config.json");
        ofs << R"({
    "provider": "",
    "saved_models": []
})";
    }

    auto cfg = acecode::load_config();

    EXPECT_TRUE(cfg.features.hooks);
}

TEST_F(ConfigFirstInitTest, ReuseOpencodeSkillsDefaultsEnabled) {
    fs::create_directories(temp_home / ".acecode");
    {
        std::ofstream ofs(temp_home / ".acecode" / "config.json");
        ofs << R"({
    "provider": "",
    "saved_models": []
})";
    }

    auto cfg = acecode::load_config();

    EXPECT_TRUE(cfg.skills.reuse_opencode);
}

TEST_F(ConfigFirstInitTest, ReuseOpencodeSkillsLoadsExplicitDisabled) {
    fs::create_directories(temp_home / ".acecode");
    {
        std::ofstream ofs(temp_home / ".acecode" / "config.json");
        ofs << R"({
    "provider": "",
    "saved_models": [],
    "skills": {
        "reuse_opencode": false
    }
})";
    }

    auto cfg = acecode::load_config();

    EXPECT_FALSE(cfg.skills.reuse_opencode);
}

TEST_F(ConfigFirstInitTest, HooksFeatureFlagLoadsExplicitDisabled) {
    fs::create_directories(temp_home / ".acecode");
    {
        std::ofstream ofs(temp_home / ".acecode" / "config.json");
        ofs << R"({
    "provider": "",
    "saved_models": [],
    "features": {
        "hooks": false
    }
})";
    }

    auto cfg = acecode::load_config();

    EXPECT_FALSE(cfg.features.hooks);
}

TEST_F(ConfigFirstInitTest, CompletedTurnSelfHealFeatureFlagDefaultsEnabled) {
    fs::create_directories(temp_home / ".acecode");
    {
        std::ofstream ofs(temp_home / ".acecode" / "config.json");
        ofs << R"({
    "provider": "",
    "saved_models": []
})";
    }

    auto cfg = acecode::load_config();

    EXPECT_TRUE(cfg.features.completed_turn_self_heal);
}

TEST_F(ConfigFirstInitTest, CompletedTurnSelfHealFeatureFlagLoadsExplicitDisabled) {
    fs::create_directories(temp_home / ".acecode");
    {
        std::ofstream ofs(temp_home / ".acecode" / "config.json");
        ofs << R"({
    "provider": "",
    "saved_models": [],
    "features": {
        "completed_turn_self_heal": false
    }
})";
    }

    auto cfg = acecode::load_config();

    EXPECT_FALSE(cfg.features.completed_turn_self_heal);
}

TEST_F(ConfigFirstInitTest, SaveConfigPersistsOnlyNonDefaultHooksFeatureFlag) {
    acecode::AppConfig cfg;
    cfg.provider = "";
    cfg.saved_models.clear();
    cfg.default_model_name.clear();

    const fs::path default_path = temp_home / ".acecode" / "default-config.json";
    acecode::save_config(cfg, default_path.string());

    {
        std::ifstream ifs(default_path);
        ASSERT_TRUE(ifs.is_open());
        auto j = nlohmann::json::parse(ifs);
        EXPECT_FALSE(j.contains("features"));
    }

    cfg.features.hooks = false;
    const fs::path disabled_path = temp_home / ".acecode" / "disabled-config.json";
    acecode::save_config(cfg, disabled_path.string());

    std::ifstream ifs(disabled_path);
    ASSERT_TRUE(ifs.is_open());
    auto j = nlohmann::json::parse(ifs);
    ASSERT_TRUE(j.contains("features"));
    EXPECT_EQ(j["features"]["hooks"], false);
}

TEST_F(ConfigFirstInitTest, SaveConfigPersistsOnlyDisabledCompletedTurnSelfHealFeatureFlag) {
    acecode::AppConfig cfg;
    cfg.provider = "";
    cfg.saved_models.clear();
    cfg.default_model_name.clear();

    const fs::path default_path = temp_home / ".acecode" / "self-heal-default-config.json";
    acecode::save_config(cfg, default_path.string());

    {
        std::ifstream ifs(default_path);
        ASSERT_TRUE(ifs.is_open());
        auto j = nlohmann::json::parse(ifs);
        EXPECT_FALSE(j.contains("features"));
    }

    cfg.features.completed_turn_self_heal = false;
    const fs::path disabled_path = temp_home / ".acecode" / "self-heal-disabled-config.json";
    acecode::save_config(cfg, disabled_path.string());

    std::ifstream ifs(disabled_path);
    ASSERT_TRUE(ifs.is_open());
    auto j = nlohmann::json::parse(ifs);
    ASSERT_TRUE(j.contains("features"));
    EXPECT_EQ(j["features"]["completed_turn_self_heal"], false);
}

TEST_F(ConfigFirstInitTest, SaveConfigOmitsDefaultReuseOpencodeSkillsFlag) {
    acecode::AppConfig cfg;
    cfg.provider = "";
    cfg.saved_models.clear();
    cfg.default_model_name.clear();
    cfg.skills.reuse_opencode = true;

    const fs::path config_path = temp_home / ".acecode" / "reuse-opencode-default.json";
    acecode::save_config(cfg, config_path.string());

    std::ifstream ifs(config_path);
    ASSERT_TRUE(ifs.is_open());
    auto j = nlohmann::json::parse(ifs);
    EXPECT_FALSE(j.contains("skills"));
}

TEST_F(ConfigFirstInitTest, SaveConfigPersistsDisabledReuseOpencodeSkillsFlag) {
    acecode::AppConfig cfg;
    cfg.provider = "";
    cfg.saved_models.clear();
    cfg.default_model_name.clear();
    cfg.skills.reuse_opencode = false;

    const fs::path config_path = temp_home / ".acecode" / "reuse-opencode-disabled.json";
    acecode::save_config(cfg, config_path.string());

    std::ifstream ifs(config_path);
    ASSERT_TRUE(ifs.is_open());
    auto j = nlohmann::json::parse(ifs);
    ASSERT_TRUE(j.contains("skills"));
    EXPECT_EQ(j["skills"]["reuse_opencode"], false);
}

TEST_F(ConfigFirstInitTest, DefaultPermissionModeLoadsAndInvalidFallsBack) {
    fs::create_directories(temp_home / ".acecode");
    {
        std::ofstream ofs(temp_home / ".acecode" / "config.json");
        ofs << R"({
    "provider": "",
    "saved_models": [],
    "default_permission_mode": "plan"
})";
    }

    auto cfg = acecode::load_config();

    EXPECT_EQ(cfg.default_permission_mode, "plan");

    {
        std::ofstream ofs(temp_home / ".acecode" / "config.json");
        ofs << R"({
    "provider": "",
    "saved_models": [],
    "default_permission_mode": "always"
})";
    }

    cfg = acecode::load_config();
    EXPECT_EQ(cfg.default_permission_mode, "default");
}

TEST_F(ConfigFirstInitTest, SavedModelsDefaultCodexFallsBackToEnabledModel) {
    fs::create_directories(temp_home / ".acecode");
    {
        std::ofstream ofs(temp_home / ".acecode" / "config.json");
        ofs << R"({
    "provider": "codex",
    "codex": { "model": "gpt-5.5" },
    "copilot": { "model": "gpt-4o" },
    "saved_models": [
        { "name": "codex", "provider": "codex", "model": "gpt-5.5" },
        { "name": "copilot", "provider": "copilot", "model": "gpt-4o" }
    ],
    "default_model_name": "codex"
})";
    }

    auto cfg = acecode::load_config();

    EXPECT_EQ(cfg.provider, "copilot");
    ASSERT_EQ(cfg.saved_models.size(), 2u);
    EXPECT_EQ(cfg.default_model_name, "copilot");
}

} // namespace
