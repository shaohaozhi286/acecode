#include "config/config.hpp"
#include "utils/paths.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
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

    (void)acecode::load_config();

    EXPECT_TRUE(fs::is_directory(temp_home / ".acecode"));
    EXPECT_TRUE(acecode::was_acecode_home_created_by_process());
    EXPECT_TRUE(acecode::consume_acecode_home_created_by_process());
    EXPECT_FALSE(acecode::was_acecode_home_created_by_process());

    (void)acecode::load_config();

    EXPECT_FALSE(acecode::consume_acecode_home_created_by_process());
}

} // namespace
