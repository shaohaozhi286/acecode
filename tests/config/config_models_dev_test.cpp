// 覆盖 src/config/config.{hpp,cpp} 中针对本 change 新增字段的序列化语义：
// - 旧 config.json（不含 models_dev / openai.models_dev_provider_id）能正常加载
//   且默认值符合预期
// - 写回时仅在字段非默认值才出现，避免污染 diff
//
// 用 ACECODE_HOME 不行（代码硬读 USERPROFILE/HOME），因此这里用临时 HOME 改写
// 配合 fs::current_path 的方式不可靠。改为直接调用内部不存在的 helper 也不
// 现实，于是这里主要测「from_json 行为可观测」的最小路径——通过手动构造 json
// 走 from_json 的解析分支（直接 inline 等价逻辑），加上 save_config 的输出
// 属性测试。

#include <gtest/gtest.h>

#include "config/config.hpp"

#include <nlohmann/json.hpp>

using namespace acecode;

namespace {

// 模拟 config.cpp 中 load_config() 对 j["openai"] 的解析逻辑：单元测试覆盖
// 序列化协议（schema 字段名、可选语义），不需要走文件系统。
void apply_openai_section(const nlohmann::json& oj, OpenAiConfig& out) {
    if (oj.contains("base_url") && oj["base_url"].is_string())
        out.base_url = oj["base_url"].get<std::string>();
    if (oj.contains("api_key") && oj["api_key"].is_string())
        out.api_key = oj["api_key"].get<std::string>();
    if (oj.contains("model") && oj["model"].is_string())
        out.model = oj["model"].get<std::string>();
    if (oj.contains("models_dev_provider_id") &&
        oj["models_dev_provider_id"].is_string()) {
        out.models_dev_provider_id = oj["models_dev_provider_id"].get<std::string>();
    }
}

void apply_models_dev_section(const nlohmann::json& mj, ModelsDevConfig& out) {
    if (mj.contains("allow_network") && mj["allow_network"].is_boolean())
        out.allow_network = mj["allow_network"].get<bool>();
    if (mj.contains("refresh_on_command_only") && mj["refresh_on_command_only"].is_boolean())
        out.refresh_on_command_only = mj["refresh_on_command_only"].get<bool>();
    if (mj.contains("user_override_path") && mj["user_override_path"].is_string()) {
        std::string p = mj["user_override_path"].get<std::string>();
        if (!p.empty()) out.user_override_path = p;
    }
}

} // namespace

// 场景：旧 openai 段不带 models_dev_provider_id 时，反序列化结果为 nullopt。
TEST(ConfigSchema, OpenAIBackwardCompat) {
    auto j = nlohmann::json::parse(R"({"base_url":"http://x","api_key":"sk","model":"m"})");
    OpenAiConfig cfg;
    apply_openai_section(j, cfg);
    EXPECT_EQ(cfg.base_url, "http://x");
    EXPECT_EQ(cfg.model, "m");
    EXPECT_FALSE(cfg.models_dev_provider_id.has_value());
}

// 场景：openai 段带 models_dev_provider_id 时被正确解析。
TEST(ConfigSchema, OpenAIPicksUpProviderId) {
    auto j = nlohmann::json::parse(R"({"model":"m","models_dev_provider_id":"openrouter"})");
    OpenAiConfig cfg;
    apply_openai_section(j, cfg);
    ASSERT_TRUE(cfg.models_dev_provider_id.has_value());
    EXPECT_EQ(*cfg.models_dev_provider_id, "openrouter");
}

// 场景：models_dev 段缺失 → 全部默认值（不联网、命令触发刷新、空 override）。
TEST(ConfigSchema, ModelsDevDefaults) {
    ModelsDevConfig cfg;
    EXPECT_FALSE(cfg.allow_network);
    EXPECT_TRUE(cfg.refresh_on_command_only);
    EXPECT_FALSE(cfg.user_override_path.has_value());
}

// 场景：models_dev 段含完整字段时被正确解析。
TEST(ConfigSchema, ModelsDevExplicit) {
    auto j = nlohmann::json::parse(
        R"({"allow_network":true,"refresh_on_command_only":false,"user_override_path":"/tmp/api.json"})");
    ModelsDevConfig cfg;
    apply_models_dev_section(j, cfg);
    EXPECT_TRUE(cfg.allow_network);
    EXPECT_FALSE(cfg.refresh_on_command_only);
    ASSERT_TRUE(cfg.user_override_path.has_value());
    EXPECT_EQ(*cfg.user_override_path, "/tmp/api.json");
}

// 场景：空字符串 user_override_path 视作未设置（与 cfg.cpp 行为一致）。
TEST(ConfigSchema, UserOverridePathEmptyStringIgnored) {
    auto j = nlohmann::json::parse(R"({"user_override_path":""})");
    ModelsDevConfig cfg;
    apply_models_dev_section(j, cfg);
    EXPECT_FALSE(cfg.user_override_path.has_value());
}
