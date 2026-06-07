#include <gtest/gtest.h>

#include "hooks/hook_config.hpp"
#include "hooks/hook_payload.hpp"

#include <string>

TEST(HookPayload, StartupBeforeModelLoadIncludesConfigPath) {
    auto payload = acecode::build_startup_before_model_load_payload("C:/work/project");

    EXPECT_EQ(payload["schema_version"], 1);
    EXPECT_EQ(payload["event"], acecode::kHookEventStartupBeforeModelLoad);
    EXPECT_EQ(payload["process"]["cwd"], "C:/work/project");
    ASSERT_TRUE(payload["config"]["path"].is_string());
    EXPECT_NE(payload["config"]["path"].get<std::string>().find("config.json"),
              std::string::npos);
}
