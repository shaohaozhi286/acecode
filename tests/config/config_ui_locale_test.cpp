#include "config/config.hpp"
#include "utils/paths.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <stdlib.h>
#endif

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
constexpr const char* kLocaleTestHomeEnv = "USERPROFILE";
#else
constexpr const char* kLocaleTestHomeEnv = "HOME";
#endif

void set_home(const std::string& value) {
#ifdef _WIN32
    _putenv_s(kLocaleTestHomeEnv, value.c_str());
#else
    setenv(kLocaleTestHomeEnv, value.c_str(), 1);
#endif
}

class ConfigUiLocaleTest : public ::testing::Test {
protected:
    fs::path root;
    std::string prior_home;
    bool had_prior_home = false;

    void SetUp() override {
        acecode::reset_run_mode_for_test();
        if (const char* value = std::getenv(kLocaleTestHomeEnv)) {
            prior_home = value;
            had_prior_home = true;
        }
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        root = fs::temp_directory_path() / ("acecode-ui-locale-" + std::to_string(tick));
        fs::create_directories(root);
        set_home(root.string());
    }

    void TearDown() override {
        if (had_prior_home) {
            set_home(prior_home);
        }
        acecode::reset_run_mode_for_test();
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    fs::path config_path() const { return root / ".acecode" / "config.json"; }

    void write_config(const nlohmann::json& value) {
        fs::create_directories(config_path().parent_path());
        std::ofstream output(config_path());
        output << value.dump(2) << '\n';
    }
};

TEST_F(ConfigUiLocaleTest, FreshConfigExplicitlyFollowsSystem) {
    const auto cfg = acecode::load_config();
    EXPECT_EQ(cfg.ui.locale, "auto");

    std::ifstream input(config_path());
    const auto saved = nlohmann::json::parse(input);
    EXPECT_EQ(saved["ui"]["locale"], "auto");
}

TEST_F(ConfigUiLocaleTest, MissingLocalePreservesLegacyChineseDefault) {
    write_config({{"provider", ""}, {"saved_models", nlohmann::json::array()}});
    EXPECT_EQ(acecode::load_config().ui.locale, "zh-CN");
}

TEST_F(ConfigUiLocaleTest, ValidValuesLoadAndInvalidValuesFallBack) {
    for (const std::string locale : {"auto", "zh-CN", "en-US"}) {
        write_config({{"provider", ""}, {"saved_models", nlohmann::json::array()},
                      {"ui", {{"locale", locale}}}});
        EXPECT_EQ(acecode::load_config().ui.locale, locale);
    }
    write_config({{"provider", ""}, {"saved_models", nlohmann::json::array()},
                  {"ui", {{"locale", "fr-FR"}}}});
    EXPECT_EQ(acecode::load_config().ui.locale, "zh-CN");
}

TEST_F(ConfigUiLocaleTest, SaveStaysSparseForLegacyChineseAndPersistsChoices) {
    acecode::AppConfig cfg;
    cfg.provider.clear();
    cfg.saved_models.clear();

    acecode::save_config(cfg, config_path().string());
    {
        std::ifstream input(config_path());
        const auto saved = nlohmann::json::parse(input);
        EXPECT_FALSE(saved.contains("ui"));
    }

    cfg.ui.locale = "auto";
    acecode::save_config(cfg, config_path().string());
    {
        std::ifstream input(config_path());
        const auto saved = nlohmann::json::parse(input);
        EXPECT_EQ(saved["ui"]["locale"], "auto");
    }

    cfg.ui.locale = "en-US";
    acecode::save_config(cfg, config_path().string());
    std::ifstream input(config_path());
    const auto saved = nlohmann::json::parse(input);
    EXPECT_EQ(saved["ui"]["locale"], "en-US");
}

TEST_F(ConfigUiLocaleTest, SaveReportsAnUnwritableTarget) {
    acecode::AppConfig cfg;
    const fs::path directory_target = root / "directory-target";
    fs::create_directories(directory_target);
    EXPECT_THROW(
        acecode::save_config(cfg, directory_target.string()),
        std::runtime_error);
}

TEST(ConfigUiLocaleValidation, AcceptsOnlyCanonicalPreferences) {
    EXPECT_TRUE(acecode::is_valid_ui_locale("auto"));
    EXPECT_TRUE(acecode::is_valid_ui_locale("zh-CN"));
    EXPECT_TRUE(acecode::is_valid_ui_locale("en-US"));
    EXPECT_FALSE(acecode::is_valid_ui_locale("en"));
    EXPECT_FALSE(acecode::is_valid_ui_locale(""));
}

} // namespace
