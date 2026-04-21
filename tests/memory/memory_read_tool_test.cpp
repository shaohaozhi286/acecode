// 覆盖 src/tool/memory_read_tool.{hpp,cpp}:
// - 无参数时返回 index + entries 列表
// - 只传 type 时按类型过滤
// - 传 name 时返回单条 + body
// - 不存在的 name 返回 {found:false} 而不是报错
// - 非法 type 值显式失败

#include <gtest/gtest.h>

#include "memory/memory_paths.hpp"
#include "memory/memory_registry.hpp"
#include "memory/memory_types.hpp"
#include "tool/memory_read_tool.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
constexpr const char* kHomeEnvName = "USERPROFILE";
#else
constexpr const char* kHomeEnvName = "HOME";
#endif

void set_env(const char* n, const std::string& v) {
#ifdef _WIN32
    _putenv_s(n, v.c_str());
#else
    setenv(n, v.c_str(), 1);
#endif
}

class MemoryReadToolTest : public ::testing::Test {
protected:
    fs::path temp_home;
    std::string prev_home;

    void SetUp() override {
        const char* e = std::getenv(kHomeEnvName);
        prev_home = e ? e : "";
        temp_home = fs::temp_directory_path() /
                    fs::path("acecode-memory-read-tool-" +
                             std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::error_code ec;
        fs::remove_all(temp_home, ec);
        fs::create_directories(temp_home);
        set_env(kHomeEnvName, temp_home.string());
        fs::create_directories(acecode::get_memory_dir());
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(temp_home, ec);
        set_env(kHomeEnvName, prev_home);
    }
};

} // namespace

// 场景:无参数调用返回所有 entry 的 name/description/type
TEST_F(MemoryReadToolTest, NoArgsReturnsAllEntries) {
    acecode::MemoryRegistry reg;
    std::string err;
    reg.upsert("a", acecode::MemoryType::User, "A desc", "bodyA",
               acecode::MemoryWriteMode::Create, err);
    reg.upsert("b", acecode::MemoryType::Feedback, "B desc", "bodyB",
               acecode::MemoryWriteMode::Create, err);

    auto tool = acecode::create_memory_read_tool(reg, 32 * 1024);
    auto r = tool.execute("{}", acecode::ToolContext{});
    ASSERT_TRUE(r.success);
    auto j = nlohmann::json::parse(r.output);
    EXPECT_EQ(j["count"].get<int>(), 2);
    EXPECT_EQ(j["entries"].size(), 2u);
    // index 字段应存在(即使为空字符串,代表 MEMORY.md 刚生成)
    EXPECT_TRUE(j.contains("index"));
}

// 场景:只传 type 时按类型过滤,且不返回 index 字段
TEST_F(MemoryReadToolTest, TypeFilterExcludesIndex) {
    acecode::MemoryRegistry reg;
    std::string err;
    reg.upsert("u1", acecode::MemoryType::User, "user1", "b",
               acecode::MemoryWriteMode::Create, err);
    reg.upsert("f1", acecode::MemoryType::Feedback, "feedback1", "b",
               acecode::MemoryWriteMode::Create, err);

    auto tool = acecode::create_memory_read_tool(reg, 32 * 1024);
    auto r = tool.execute(R"({"type":"feedback"})", acecode::ToolContext{});
    ASSERT_TRUE(r.success);
    auto j = nlohmann::json::parse(r.output);
    EXPECT_EQ(j["count"].get<int>(), 1);
    EXPECT_EQ(j["entries"][0]["name"], "f1");
    EXPECT_FALSE(j.contains("index"));
}

// 场景:传 name 时返回完整 body
TEST_F(MemoryReadToolTest, NameReturnsFullBody) {
    acecode::MemoryRegistry reg;
    std::string err;
    reg.upsert("pick", acecode::MemoryType::Project, "pick desc", "# my body\n",
               acecode::MemoryWriteMode::Create, err);

    auto tool = acecode::create_memory_read_tool(reg, 32 * 1024);
    auto r = tool.execute(R"({"name":"pick"})", acecode::ToolContext{});
    ASSERT_TRUE(r.success);
    auto j = nlohmann::json::parse(r.output);
    EXPECT_TRUE(j["found"].get<bool>());
    EXPECT_EQ(j["name"], "pick");
    EXPECT_NE(j["body"].get<std::string>().find("my body"), std::string::npos);
}

// 场景:name 找不到时返回 {found:false},不报错
TEST_F(MemoryReadToolTest, MissingNameReturnsNotFoundNotError) {
    acecode::MemoryRegistry reg;
    reg.scan();

    auto tool = acecode::create_memory_read_tool(reg, 32 * 1024);
    auto r = tool.execute(R"({"name":"ghost"})", acecode::ToolContext{});
    ASSERT_TRUE(r.success);
    auto j = nlohmann::json::parse(r.output);
    EXPECT_FALSE(j["found"].get<bool>());
}

// 场景:非法 type 枚举值返回 success=false
TEST_F(MemoryReadToolTest, InvalidTypeIsError) {
    acecode::MemoryRegistry reg;
    reg.scan();
    auto tool = acecode::create_memory_read_tool(reg, 32 * 1024);
    auto r = tool.execute(R"({"type":"notes"})", acecode::ToolContext{});
    EXPECT_FALSE(r.success);
}
