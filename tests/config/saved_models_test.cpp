// 覆盖 src/config/saved_models.{hpp,cpp} 的纯函数 parse + validate。
// 对应 openspec/changes/model-profiles 的任务 7.1-7.7。
// 文件头与每个 TEST 都加中文注释,遵循 feedback_unit_test_chinese_comments 约定。

#include <gtest/gtest.h>

#include "config/saved_models.hpp"

#include <nlohmann/json.hpp>

using namespace acecode;

// 7.2 — 合法 saved_models 最小输入(1 个 openai entry,所有字段齐全)→ validate 通过。
TEST(SavedModelsTest, MinimalValidOpenaiEntryPassesValidation) {
    nlohmann::json j = nlohmann::json::array();
    j.push_back({
        {"name", "local-lm"},
        {"provider", "openai"},
        {"base_url", "http://localhost:1234/v1"},
        {"api_key", "x"},
        {"model", "llama-3"}
    });

    std::string err;
    auto parsed = parse_saved_models(j, err);
    ASSERT_TRUE(parsed.has_value()) << err;
    ASSERT_EQ(parsed->size(), 1u);
    EXPECT_EQ((*parsed)[0].name, "local-lm");

    err.clear();
    EXPECT_TRUE(validate_saved_models(*parsed, "local-lm", err)) << err;
    EXPECT_TRUE(err.empty());
}

// 7.3 — 两个 entry 重复 name → 校验失败,err 含 "duplicate"。
TEST(SavedModelsTest, DuplicateNameFails) {
    std::vector<ModelProfile> entries;
    ModelProfile a;
    a.name = "dup";
    a.provider = "openai";
    a.base_url = "http://x";
    a.api_key = "k";
    a.model = "m1";
    ModelProfile b = a;
    b.model = "m2";
    entries.push_back(a);
    entries.push_back(b);

    std::string err;
    EXPECT_FALSE(validate_saved_models(entries, "", err));
    EXPECT_NE(err.find("duplicate"), std::string::npos) << err;
    EXPECT_NE(err.find("dup"), std::string::npos) << err;
}

// 7.4 — name 以 `(` 开头 → 校验失败,err 含 "reserved prefix"。
TEST(SavedModelsTest, ReservedPrefixFails) {
    std::vector<ModelProfile> entries;
    ModelProfile e;
    e.name = "(mine)";
    e.provider = "openai";
    e.base_url = "http://x";
    e.api_key = "k";
    e.model = "m";
    entries.push_back(e);

    std::string err;
    EXPECT_FALSE(validate_saved_models(entries, "", err));
    EXPECT_NE(err.find("reserved prefix"), std::string::npos) << err;
}

// 7.5 — openai entry 缺 base_url → 校验失败,err 含 "base_url"。
TEST(SavedModelsTest, OpenaiMissingBaseUrlFails) {
    std::vector<ModelProfile> entries;
    ModelProfile e;
    e.name = "x";
    e.provider = "openai";
    e.api_key = "k";  // base_url 故意留空
    e.model = "y";
    entries.push_back(e);

    std::string err;
    EXPECT_FALSE(validate_saved_models(entries, "", err));
    EXPECT_NE(err.find("base_url"), std::string::npos) << err;
}

// 7.6 — default_model_name 指向不在 saved_models 里的 name → 校验失败,err 含
// "default_model_name"。
TEST(SavedModelsTest, DefaultNameNotInListFails) {
    std::vector<ModelProfile> entries;
    ModelProfile e;
    e.name = "real";
    e.provider = "copilot";
    e.model = "gpt-4o";
    entries.push_back(e);

    std::string err;
    EXPECT_FALSE(validate_saved_models(entries, "ghost", err));
    EXPECT_NE(err.find("default_model_name"), std::string::npos) << err;
    EXPECT_NE(err.find("ghost"), std::string::npos) << err;
}

// 7.7 — 空 saved_models + 空 default → 校验通过(纯空配置合法,等价于完全
// 不设 model profiles,运行时走 legacy 兜底)。
TEST(SavedModelsTest, EmptyConfigurationPasses) {
    std::vector<ModelProfile> entries;
    std::string err;
    EXPECT_TRUE(validate_saved_models(entries, "", err)) << err;
    EXPECT_TRUE(err.empty());
}

// 额外 — copilot entry 不需要 base_url / api_key 也能通过校验。
TEST(SavedModelsTest, CopilotEntryWithoutBaseUrlPasses) {
    std::vector<ModelProfile> entries;
    ModelProfile e;
    e.name = "copilot-fast";
    e.provider = "copilot";
    e.model = "gpt-4o";
    entries.push_back(e);

    std::string err;
    EXPECT_TRUE(validate_saved_models(entries, "copilot-fast", err)) << err;
}

// 额外 — parse_saved_models 拒绝非数组的输入。
TEST(SavedModelsTest, ParseRejectsNonArray) {
    nlohmann::json j = nlohmann::json::object();
    j["foo"] = "bar";

    std::string err;
    auto parsed = parse_saved_models(j, err);
    EXPECT_FALSE(parsed.has_value());
    EXPECT_NE(err.find("array"), std::string::npos) << err;
}

// 额外 — parse_saved_models 把 null 视为空数组(向后兼容缺字段写法)。
TEST(SavedModelsTest, ParseTreatsNullAsEmpty) {
    nlohmann::json j = nullptr;
    std::string err;
    auto parsed = parse_saved_models(j, err);
    ASSERT_TRUE(parsed.has_value()) << err;
    EXPECT_TRUE(parsed->empty());
}
