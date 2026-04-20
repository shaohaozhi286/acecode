// 覆盖 src/provider/models_dev_registry.{hpp,cpp}：
// - validate_registry_schema 的最小校验语义（顶层 object + 至少一个 provider 含 models）
// - find_provider_entry 大小写不敏感
// - initialize_registry/reload_registry_from_disk：user_override > bundled > empty
// - 损坏 JSON 不抛异常并降级到 empty 源

#include <gtest/gtest.h>

#include "provider/models_dev_registry.hpp"
#include "provider/models_dev_paths.hpp"
#include "config/config.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
void set_env(const char* k, const char* v) { _putenv_s(k, v ? v : ""); }
#else
void set_env(const char* k, const char* v) {
    if (v) ::setenv(k, v, 1);
    else ::unsetenv(k);
}
#endif

fs::path tmp_dir(const std::string& tag) {
    auto p = fs::temp_directory_path() / ("acecode_reg_" + tag);
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

void write_file(const fs::path& p, const std::string& contents) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << contents;
}

const char* kMinimalRegistry =
    R"({"anthropic":{"id":"anthropic","name":"Anthropic","models":{"claude":{"id":"claude","limit":{"context":200000}}}}})";

} // namespace

// 场景：合法注册表（顶层 object + 至少一个 provider 有非空 models）。
TEST(ModelsDevRegistry, ValidateAcceptsObjectWithModels) {
    auto j = nlohmann::json::parse(kMinimalRegistry);
    EXPECT_TRUE(acecode::validate_registry_schema(j));
}

// 场景：顶层数组、空对象、provider 无 models 字段 → 全部判为非法。
TEST(ModelsDevRegistry, ValidateRejectsBadShapes) {
    EXPECT_FALSE(acecode::validate_registry_schema(nlohmann::json::array()));
    EXPECT_FALSE(acecode::validate_registry_schema(nlohmann::json::object()));
    auto no_models = nlohmann::json::parse(R"({"x":{"name":"foo"}})");
    EXPECT_FALSE(acecode::validate_registry_schema(no_models));
    auto empty_models = nlohmann::json::parse(R"({"x":{"models":{}}})");
    EXPECT_FALSE(acecode::validate_registry_schema(empty_models));
}

// 场景：find_provider_entry 应忽略大小写差异。
TEST(ModelsDevRegistry, FindProviderIsCaseInsensitive) {
    auto j = nlohmann::json::parse(kMinimalRegistry);
    EXPECT_NE(nullptr, acecode::find_provider_entry(j, "ANTHROPIC"));
    EXPECT_NE(nullptr, acecode::find_provider_entry(j, "Anthropic"));
    EXPECT_EQ(nullptr, acecode::find_provider_entry(j, "openai"));
    EXPECT_EQ(nullptr, acecode::find_provider_entry(j, ""));
}

// 场景：initialize_registry 命中 user_override 时跳过 bundled。
TEST(ModelsDevRegistry, UserOverrideBeatsBundled) {
    auto override_dir = tmp_dir("override");
    auto override_path = override_dir / "custom.json";
    write_file(override_path, R"({"openai":{"id":"openai","models":{"gpt":{"id":"gpt","limit":{"context":4096}}}}})");

    auto seed = tmp_dir("seed");
    write_file(seed / "api.json", kMinimalRegistry);

    set_env("ACECODE_MODELS_DEV_DIR", seed.string().c_str());

    acecode::AppConfig cfg;
    cfg.models_dev.user_override_path = override_path.string();
    acecode::initialize_registry(cfg, "");

    auto reg = acecode::current_registry();
    ASSERT_TRUE(reg);
    EXPECT_TRUE(reg->contains("openai"));
    EXPECT_FALSE(reg->contains("anthropic"));
    EXPECT_EQ(acecode::current_registry_source().kind,
              acecode::RegistrySource::Kind::UserOverride);

    // cleanup so other tests start fresh
    set_env("ACECODE_MODELS_DEV_DIR", nullptr);
}

// 场景：user_override 不存在时回退到 bundled，并设置正确的 RegistrySource。
TEST(ModelsDevRegistry, FallsBackToBundled) {
    auto seed = tmp_dir("seed_only");
    write_file(seed / "api.json", kMinimalRegistry);
    write_file(seed / "MANIFEST.json", R"({"upstream_commit":"abc"})");
    set_env("ACECODE_MODELS_DEV_DIR", seed.string().c_str());

    acecode::AppConfig cfg;  // no user_override
    acecode::initialize_registry(cfg, "");

    auto reg = acecode::current_registry();
    ASSERT_TRUE(reg);
    EXPECT_TRUE(reg->contains("anthropic"));

    const auto& src = acecode::current_registry_source();
    EXPECT_EQ(src.kind, acecode::RegistrySource::Kind::Bundled);
    ASSERT_TRUE(src.manifest.has_value());
    EXPECT_EQ((*src.manifest)["upstream_commit"], "abc");

    set_env("ACECODE_MODELS_DEV_DIR", nullptr);
}

// 场景：损坏 JSON 不抛异常，降级到 empty 注册表，但仍能查询（返回空）。
TEST(ModelsDevRegistry, CorruptJsonGracefullyEmpty) {
    auto seed = tmp_dir("corrupt");
    write_file(seed / "api.json", "not-json{");
    set_env("ACECODE_MODELS_DEV_DIR", seed.string().c_str());

    acecode::AppConfig cfg;
    EXPECT_NO_THROW(acecode::initialize_registry(cfg, ""));

    auto reg = acecode::current_registry();
    ASSERT_TRUE(reg);
    EXPECT_TRUE(reg->empty());
    EXPECT_EQ(acecode::current_registry_source().kind,
              acecode::RegistrySource::Kind::Empty);

    set_env("ACECODE_MODELS_DEV_DIR", nullptr);
}
