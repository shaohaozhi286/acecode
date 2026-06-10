// 覆盖 src/config/saved_models.{hpp,cpp} 的纯函数 parse + validate。
// 对应 openspec/changes/model-profiles 的任务 7.1-7.7。
// 文件头与每个 TEST 都加中文注释,遵循 feedback_unit_test_chinese_comments 约定。

#include <gtest/gtest.h>

#include "config/config.hpp"
#include "config/saved_models.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

using namespace acecode;

// 额外 — OpenAI 兼容请求默认超时必须是 666 秒。
TEST(SavedModelsTest, OpenAiStreamTimeoutDefaultIs666Seconds) {
    EXPECT_EQ(OpenAiConfig::kDefaultStreamTimeoutMs, 666000);

    AppConfig cfg;
    EXPECT_EQ(cfg.openai.stream_timeout_ms, 666000);
}

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

// 7.7 — 空 saved_models + 空 default → 校验通过。运行入口会从旧 schema
// provider/openai/copilot 字段合成临时模型兜底。
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

// 额外 — codex entry 与 copilot 一样不需要 base_url / api_key。
TEST(SavedModelsTest, CodexEntryWithoutBaseUrlPasses) {
    nlohmann::json j = nlohmann::json::array();
    j.push_back({
        {"name", "codex"},
        {"provider", "codex"},
        {"model", "gpt-5.5"}
    });

    std::string err;
    auto parsed = parse_saved_models(j, err);
    ASSERT_TRUE(parsed.has_value()) << err;
    ASSERT_EQ(parsed->size(), 1u);
    EXPECT_EQ((*parsed)[0].provider, "codex");

    err.clear();
    EXPECT_TRUE(validate_saved_models(*parsed, "codex", err)) << err;
}

// 额外 — codex entry 缺 model 仍然在 parse 阶段拒绝。
TEST(SavedModelsTest, CodexEntryMissingModelFails) {
    nlohmann::json j = nlohmann::json::array();
    j.push_back({
        {"name", "codex"},
        {"provider", "codex"}
    });

    std::string err;
    auto parsed = parse_saved_models(j, err);
    EXPECT_FALSE(parsed.has_value());
    EXPECT_NE(err.find("model"), std::string::npos) << err;
}

// 额外 — context_window 是可选正整数;解析后参与 validate。
TEST(SavedModelsTest, OptionalContextWindowParsesAndValidates) {
    nlohmann::json j = nlohmann::json::array();
    j.push_back({
        {"name", "local-lm"},
        {"provider", "openai"},
        {"base_url", "http://localhost:1234/v1"},
        {"api_key", "x"},
        {"model", "llama-3"},
        {"context_window", 64000}
    });

    std::string err;
    auto parsed = parse_saved_models(j, err);
    ASSERT_TRUE(parsed.has_value()) << err;
    ASSERT_EQ(parsed->size(), 1u);
    ASSERT_TRUE((*parsed)[0].context_window.has_value());
    EXPECT_EQ(*(*parsed)[0].context_window, 64000);

    err.clear();
    EXPECT_TRUE(validate_saved_models(*parsed, "local-lm", err)) << err;
}

// 额外 — stream_timeout_ms 是可选正整数;解析后参与 validate。
TEST(SavedModelsTest, OptionalStreamTimeoutParsesAndValidates) {
    nlohmann::json j = nlohmann::json::array();
    j.push_back({
        {"name", "local-lm"},
        {"provider", "openai"},
        {"base_url", "http://localhost:1234/v1"},
        {"api_key", "x"},
        {"model", "llama-3"},
        {"stream_timeout_ms", 300000}
    });

    std::string err;
    auto parsed = parse_saved_models(j, err);
    ASSERT_TRUE(parsed.has_value()) << err;
    ASSERT_EQ(parsed->size(), 1u);
    ASSERT_TRUE((*parsed)[0].stream_timeout_ms.has_value());
    EXPECT_EQ(*(*parsed)[0].stream_timeout_ms, 300000);

    err.clear();
    EXPECT_TRUE(validate_saved_models(*parsed, "local-lm", err)) << err;
}

// 额外 — capabilities 是可选标签数组;解析后参与 validate 并允许未知标签。
TEST(SavedModelsTest, OptionalCapabilitiesParseAndValidate) {
    nlohmann::json j = nlohmann::json::array();
    j.push_back({
        {"name", "vision-lm"},
        {"provider", "openai"},
        {"base_url", "http://localhost:1234/v1"},
        {"api_key", "x"},
        {"model", "llava"},
        {"capabilities", nlohmann::json::array({"vision", "tool_use", "custom_capability"})}
    });

    std::string err;
    auto parsed = parse_saved_models(j, err);
    ASSERT_TRUE(parsed.has_value()) << err;
    ASSERT_EQ(parsed->size(), 1u);
    EXPECT_EQ((*parsed)[0].capabilities,
              (std::vector<std::string>{"vision", "tool_use", "custom_capability"}));

    err.clear();
    EXPECT_TRUE(validate_saved_models(*parsed, "vision-lm", err)) << err;
}

// 额外 — request_headers 是可选 JSON object,值保持模板形态不解析环境变量。
TEST(SavedModelsTest, OptionalRequestHeadersParseAndValidate) {
    nlohmann::json j = nlohmann::json::array();
    j.push_back({
        {"name", "local-lm"},
        {"provider", "openai"},
        {"base_url", "http://localhost:1234/v1"},
        {"api_key", "x"},
        {"model", "llama-3"},
        {"request_headers", {
            {"X-Team", "acecode"},
            {"X-Token", "{env:ACE_TOKEN}"}
        }}
    });

    std::string err;
    auto parsed = parse_saved_models(j, err);
    ASSERT_TRUE(parsed.has_value()) << err;
    ASSERT_EQ(parsed->size(), 1u);
    EXPECT_EQ((*parsed)[0].request_headers.at("X-Team"), "acecode");
    EXPECT_EQ((*parsed)[0].request_headers.at("X-Token"), "{env:ACE_TOKEN}");

    err.clear();
    EXPECT_TRUE(validate_saved_models(*parsed, "local-lm", err)) << err;
}

// 额外 — 旧式 openai 段里的 request_headers 迁移到合成的 saved model。
TEST(SavedModelsTest, LegacyOpenAiProfileCarriesGlobalRequestHeaders) {
    AppConfig cfg;
    cfg.provider = "openai";
    cfg.openai.base_url = "http://localhost:1234/v1";
    cfg.openai.api_key = "sk-test";
    cfg.openai.model = "llama-3";
    cfg.openai.request_headers = {
        {"X-Team", "acecode"},
        {"X-Token", "{env:ACE_TOKEN}"}
    };

    ModelProfile profile = legacy_model_profile_from_config(cfg);

    EXPECT_EQ(profile.provider, "openai");
    EXPECT_EQ(profile.request_headers.at("X-Team"), "acecode");
    EXPECT_EQ(profile.request_headers.at("X-Token"), "{env:ACE_TOKEN}");
}

// 额外 — request_headers 不能覆盖 ACECode 固定管理的 Content-Type。
TEST(SavedModelsTest, RequestHeadersRejectContentType) {
    ModelProfile e;
    e.name = "local";
    e.provider = "openai";
    e.base_url = "http://localhost:1234/v1";
    e.api_key = "x";
    e.model = "llama-3";
    e.request_headers = {{"Content-Type", "text/plain"}};

    std::string err;
    EXPECT_FALSE(validate_saved_models({e}, "", err));
    EXPECT_NE(err.find("request_headers"), std::string::npos) << err;
    EXPECT_NE(err.find("Content-Type"), std::string::npos) << err;
}

// 额外 — malformed {env:...} 占位符必须在配置校验阶段拒绝。
TEST(SavedModelsTest, RequestHeadersRejectMalformedEnvPlaceholder) {
    ModelProfile e;
    e.name = "local";
    e.provider = "openai";
    e.base_url = "http://localhost:1234/v1";
    e.api_key = "x";
    e.model = "llama-3";
    e.request_headers = {{"X-Token", "{env:}"}};

    std::string err;
    EXPECT_FALSE(validate_saved_models({e}, "", err));
    EXPECT_NE(err.find("request_headers"), std::string::npos) << err;
}

// 额外 — request_headers 只属于 OpenAI-compatible saved model。
TEST(SavedModelsTest, RequestHeadersRejectNonOpenAiProvider) {
    ModelProfile e;
    e.name = "copilot-fast";
    e.provider = "copilot";
    e.model = "gpt-4o";
    e.request_headers = {{"X-Team", "acecode"}};

    std::string err;
    EXPECT_FALSE(validate_saved_models({e}, "", err));
    EXPECT_NE(err.find("request_headers"), std::string::npos) << err;
    EXPECT_NE(err.find("provider"), std::string::npos) << err;
}

// 额外 — capabilities 内重复标签不通过 validate,避免路由/搜索出现歧义。
TEST(SavedModelsTest, DuplicateCapabilitiesFailValidation) {
    ModelProfile e;
    e.name = "local";
    e.provider = "copilot";
    e.model = "gpt-4o";
    e.capabilities = {"vision", "vision"};

    std::string err;
    EXPECT_FALSE(validate_saved_models({e}, "", err));
    EXPECT_NE(err.find("duplicate capability"), std::string::npos) << err;
}

// 额外 — 手工构造的无效 context_window 不能通过 validate。
TEST(SavedModelsTest, InvalidContextWindowFailsValidation) {
    ModelProfile e;
    e.name = "local";
    e.provider = "copilot";
    e.model = "gpt-4o";
    e.context_window = 0;

    std::string err;
    EXPECT_FALSE(validate_saved_models({e}, "", err));
    EXPECT_NE(err.find("context_window"), std::string::npos) << err;
}

// 额外 — 手工构造的无效 stream_timeout_ms 不能通过 validate。
TEST(SavedModelsTest, InvalidStreamTimeoutFailsValidation) {
    ModelProfile e;
    e.name = "local";
    e.provider = "copilot";
    e.model = "gpt-4o";
    e.stream_timeout_ms = 0;

    std::string err;
    EXPECT_FALSE(validate_saved_models({e}, "", err));
    EXPECT_NE(err.find("stream_timeout_ms"), std::string::npos) << err;
}

// 额外 — capabilities 内控制字符不通过 validate。
TEST(SavedModelsTest, InvalidCapabilityFailsValidation) {
    ModelProfile e;
    e.name = "local";
    e.provider = "copilot";
    e.model = "gpt-4o";
    e.capabilities = {std::string("vision\nbad")};

    std::string err;
    EXPECT_FALSE(validate_saved_models({e}, "", err));
    EXPECT_NE(err.find("capability"), std::string::npos) << err;
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

// 额外 — save_config 把 per-model context_window 写回 saved_models entry。
TEST(SavedModelsTest, SaveConfigPersistsContextWindow) {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
        ("acecode-saved-model-context-window-" + std::to_string(suffix) + ".json");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    AppConfig cfg;
    ModelProfile e;
    e.name = "local";
    e.provider = "copilot";
    e.model = "gpt-4o";
    e.context_window = 64000;
    cfg.saved_models.push_back(e);
    cfg.default_model_name = "local";
    save_config(cfg, path.string());

    std::ifstream ifs(path);
    ASSERT_TRUE(ifs.is_open());
    const auto saved = nlohmann::json::parse(ifs);
    ASSERT_TRUE(saved.contains("saved_models"));
    ASSERT_EQ(saved["saved_models"].size(), 1u);
    EXPECT_EQ(saved["saved_models"][0]["context_window"], 64000);

    std::filesystem::remove(path, ec);
}

// 额外 — save_config 写回全局 openai.stream_timeout_ms 与 per-model override。
TEST(SavedModelsTest, SaveConfigPersistsStreamTimeouts) {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
        ("acecode-stream-timeout-" + std::to_string(suffix) + ".json");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    AppConfig cfg;
    cfg.openai.stream_timeout_ms = 300000;
    ModelProfile e;
    e.name = "local";
    e.provider = "openai";
    e.base_url = "http://localhost:1234/v1";
    e.api_key = "sk-test";
    e.model = "llama-3";
    e.stream_timeout_ms = 450000;
    cfg.saved_models.push_back(e);
    cfg.default_model_name = "local";
    save_config(cfg, path.string());

    std::ifstream ifs(path);
    ASSERT_TRUE(ifs.is_open());
    const auto saved = nlohmann::json::parse(ifs);
    ASSERT_TRUE(saved.contains("openai"));
    EXPECT_EQ(saved["openai"]["stream_timeout_ms"], 300000);
    ASSERT_TRUE(saved.contains("saved_models"));
    ASSERT_EQ(saved["saved_models"].size(), 1u);
    EXPECT_EQ(saved["saved_models"][0]["stream_timeout_ms"], 450000);

    std::filesystem::remove(path, ec);
}

// 额外 — save_config 把 per-model capabilities 写回 saved_models entry。
TEST(SavedModelsTest, SaveConfigPersistsCapabilities) {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
        ("acecode-capabilities-" + std::to_string(suffix) + ".json");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    AppConfig cfg;
    ModelProfile e;
    e.name = "vision";
    e.provider = "copilot";
    e.model = "gpt-4o";
    e.capabilities = {"vision", "tool_use"};
    cfg.saved_models.push_back(e);
    cfg.default_model_name = "vision";
    save_config(cfg, path.string());

    std::ifstream ifs(path);
    ASSERT_TRUE(ifs.is_open());
    const auto saved = nlohmann::json::parse(ifs);
    ASSERT_TRUE(saved.contains("saved_models"));
    ASSERT_EQ(saved["saved_models"].size(), 1u);
    EXPECT_EQ(saved["saved_models"][0]["capabilities"],
              nlohmann::json::array({"vision", "tool_use"}));

    std::filesystem::remove(path, ec);
}

// 额外 — save_config 把 request_headers 模板原样写回 saved_models entry。
TEST(SavedModelsTest, SaveConfigPersistsRequestHeaders) {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
        ("acecode-request-headers-" + std::to_string(suffix) + ".json");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    AppConfig cfg;
    ModelProfile e;
    e.name = "local";
    e.provider = "openai";
    e.base_url = "http://localhost:1234/v1";
    e.api_key = "sk-test";
    e.model = "llama-3";
    e.request_headers = {
        {"Authorization", "Bearer {env:ACE_TOKEN}"},
        {"X-Team", "acecode"}
    };
    cfg.saved_models.push_back(e);
    cfg.default_model_name = "local";
    save_config(cfg, path.string());

    std::ifstream ifs(path);
    ASSERT_TRUE(ifs.is_open());
    const auto saved = nlohmann::json::parse(ifs);
    ASSERT_TRUE(saved.contains("saved_models"));
    ASSERT_EQ(saved["saved_models"].size(), 1u);
    ASSERT_TRUE(saved["saved_models"][0].contains("request_headers"));
    EXPECT_EQ(saved["saved_models"][0]["request_headers"]["Authorization"],
              "Bearer {env:ACE_TOKEN}");
    EXPECT_EQ(saved["saved_models"][0]["request_headers"]["X-Team"], "acecode");

    std::filesystem::remove(path, ec);
}

// 额外 — save_config 保留 openai.request_headers,避免配置向导保存后丢全局 header。
TEST(SavedModelsTest, SaveConfigPersistsGlobalOpenAiRequestHeaders) {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
        ("acecode-global-request-headers-" + std::to_string(suffix) + ".json");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    AppConfig cfg;
    cfg.openai.request_headers = {
        {"X-Team", "acecode"},
        {"X-Token", "{env:ACE_TOKEN}"}
    };
    save_config(cfg, path.string());

    std::ifstream ifs(path);
    ASSERT_TRUE(ifs.is_open());
    const auto saved = nlohmann::json::parse(ifs);
    ASSERT_TRUE(saved["openai"].contains("request_headers"));
    EXPECT_EQ(saved["openai"]["request_headers"]["X-Team"], "acecode");
    EXPECT_EQ(saved["openai"]["request_headers"]["X-Token"], "{env:ACE_TOKEN}");

    std::filesystem::remove(path, ec);
}

// 额外 — validate_config 拒绝非正数的全局 OpenAI stream timeout。
TEST(SavedModelsTest, ValidateConfigRejectsInvalidOpenAiStreamTimeout) {
    AppConfig cfg;
    cfg.openai.stream_timeout_ms = 0;

    auto errors = validate_config(cfg);
    ASSERT_FALSE(errors.empty());
    bool found = false;
    for (const auto& err : errors) {
        if (err.find("openai.stream_timeout_ms") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}
