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
    bool had_previous_home = false;

    void SetUp() override {
        acecode::reset_run_mode_for_test();
        acecode::reset_acecode_home_created_flag_for_test();
        if (const char* existing = std::getenv(kHomeEnvName)) {
            previous_home = existing;
            had_previous_home = true;
        }
        temp_home = make_temp_root();
        set_env_value(kHomeEnvName, temp_home.string());
    }

    void TearDown() override {
        if (had_previous_home) {
            set_env_value(kHomeEnvName, previous_home);
        } else {
            clear_env_value(kHomeEnvName);
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

    ASSERT_EQ(cfg.saved_models.size(), 1u);
    EXPECT_EQ(cfg.saved_models[0].name, "copilot");
    EXPECT_EQ(cfg.saved_models[0].provider, "copilot");
    EXPECT_EQ(cfg.saved_models[0].model, "gpt-4o");
    EXPECT_EQ(cfg.default_model_name, "copilot");

    std::ifstream ifs(temp_home / ".acecode" / "config.json");
    ASSERT_TRUE(ifs.is_open());
    auto j = nlohmann::json::parse(ifs);
    ASSERT_TRUE(j.contains("saved_models"));
    ASSERT_TRUE(j["saved_models"].is_array());
    ASSERT_EQ(j["saved_models"].size(), 1u);
    EXPECT_EQ(j["saved_models"][0]["name"], "copilot");
    EXPECT_EQ(j["default_model_name"], "copilot");
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

} // namespace
