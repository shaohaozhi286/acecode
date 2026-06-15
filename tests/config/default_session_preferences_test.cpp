#include <gtest/gtest.h>

#include "config/config.hpp"

#include <filesystem>
#include <random>

namespace fs = std::filesystem;

namespace {

acecode::ModelProfile test_model(const std::string& name,
                                  const std::string& model) {
    acecode::ModelProfile profile;
    profile.name = name;
    profile.provider = "copilot";
    profile.model = model;
    return profile;
}

fs::path temp_config_path() {
    return fs::temp_directory_path() /
           ("acecode_default_session_preferences_" +
            std::to_string(std::random_device{}()) + ".json");
}

} // namespace

TEST(DefaultSessionPreferencesConfig, RefreshReadsExternalModelAndPermissionDefaults) {
    acecode::AppConfig runtime;
    runtime.saved_models = {test_model("fast", "gpt-fast")};
    runtime.default_model_name = "fast";
    runtime.default_permission_mode = "default";

    acecode::AppConfig disk = runtime;
    disk.saved_models.push_back(test_model("slow", "gpt-slow"));
    disk.default_model_name = "slow";
    disk.default_permission_mode = "plan";

    const auto path = temp_config_path();
    acecode::save_config(disk, path.string());

    std::string error;
    ASSERT_TRUE(acecode::refresh_default_session_preferences_from_config(
        runtime, path.string(), &error)) << error;

    ASSERT_EQ(runtime.saved_models.size(), 2u);
    EXPECT_EQ(runtime.default_model_name, "slow");
    EXPECT_EQ(runtime.default_permission_mode, "plan");
    EXPECT_EQ(runtime.saved_models[1].name, "slow");

    std::error_code ec;
    fs::remove(path, ec);
}

