#include "config/config.hpp"
#include "utils/paths.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>

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

fs::path make_temp_home() {
    return fs::temp_directory_path() /
           ("acecode-upgrade-config-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

} // namespace

TEST(ConfigUpgrade, DefaultsNormalizeAndValidate) {
    acecode::AppConfig cfg;
    EXPECT_EQ(cfg.upgrade.base_url, "http://2017studio.imwork.net:82/aupdate/");
    EXPECT_EQ(cfg.upgrade.timeout_ms, 30000);
    EXPECT_EQ(acecode::normalize_upgrade_base_url(" https://u.test/base "),
              "https://u.test/base/");
    EXPECT_TRUE(acecode::is_valid_upgrade_base_url("http://u.test/base"));

    cfg.upgrade.base_url = "ftp://bad/";
    auto errors = acecode::validate_config(cfg);
    bool found = false;
    for (const auto& e : errors) {
        if (e.find("upgrade.base_url") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(ConfigUpgrade, LoadConfigReadsUpgradeAndEnvOverride) {
    acecode::reset_run_mode_for_test();
    fs::path temp_home = make_temp_home();
    std::string previous_home;
    bool had_previous_home = false;
    if (const char* existing = std::getenv(kHomeEnvName)) {
        previous_home = existing;
        had_previous_home = true;
    }
    set_env_value(kHomeEnvName, temp_home.string());
    set_env_value("ACECODE_UPGRADE_BASE_URL", "https://env.example.test/update");

    fs::create_directories(temp_home / ".acecode");
    {
        std::ofstream ofs(temp_home / ".acecode" / "config.json");
        ofs << R"({"upgrade":{"base_url":"https://file.example.test/root","timeout_ms":45000}})";
    }

    acecode::AppConfig cfg = acecode::load_config();
    EXPECT_EQ(cfg.upgrade.base_url, "https://env.example.test/update/");
    EXPECT_EQ(cfg.upgrade.timeout_ms, 45000);

    clear_env_value("ACECODE_UPGRADE_BASE_URL");
    if (had_previous_home) set_env_value(kHomeEnvName, previous_home);
    else clear_env_value(kHomeEnvName);
    std::error_code ec;
    fs::remove_all(temp_home, ec);
    acecode::reset_run_mode_for_test();
}
