// 覆盖 src/tool/memory_write_tool.{hpp,cpp}:
// - 合法参数 upsert 成功并落盘
// - 必填字段缺失返回 success=false
// - name 含非法字符被拒
// - mode=create 遇已有 entry 失败
// - mode=update 遇不存在 entry 失败

#include <gtest/gtest.h>

#include "memory/memory_paths.hpp"
#include "memory/memory_registry.hpp"
#include "memory/memory_types.hpp"
#include "tool/memory_write_tool.hpp"

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

class MemoryWriteToolTest : public ::testing::Test {
protected:
    fs::path temp_home;
    std::string prev_home;

    void SetUp() override {
        const char* e = std::getenv(kHomeEnvName);
        prev_home = e ? e : "";
        temp_home = fs::temp_directory_path() /
                    fs::path("acecode-memory-write-tool-" +
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

// 场景:合法参数的 upsert 成功,文件真的出现在 memory 目录里
TEST_F(MemoryWriteToolTest, UpsertHappyPath) {
    acecode::MemoryRegistry reg;
    reg.scan();
    auto tool = acecode::create_memory_write_tool(reg);

    std::string args = R"({"name":"foo","type":"user","description":"foo desc","body":"body"})";
    auto r = tool.execute(args, acecode::ToolContext{});
    ASSERT_TRUE(r.success) << r.output;
    auto j = nlohmann::json::parse(r.output);
    EXPECT_TRUE(j["success"].get<bool>());
    EXPECT_EQ(j["name"], "foo");
    EXPECT_TRUE(fs::exists(acecode::get_memory_dir() / "foo.md"));
}

// 场景:缺少必填字段 description 时返回错误
TEST_F(MemoryWriteToolTest, MissingDescriptionFails) {
    acecode::MemoryRegistry reg;
    auto tool = acecode::create_memory_write_tool(reg);
    auto r = tool.execute(R"({"name":"foo","type":"user","body":"b"})",
                          acecode::ToolContext{});
    EXPECT_FALSE(r.success);
}

// 场景:name 含路径穿越被 validate_memory_name 拒绝,不触碰文件系统
TEST_F(MemoryWriteToolTest, PathTraversalRejected) {
    acecode::MemoryRegistry reg;
    reg.scan();
    auto tool = acecode::create_memory_write_tool(reg);
    auto r = tool.execute(
        R"({"name":"../etc/passwd","type":"user","description":"x","body":"y"})",
        acecode::ToolContext{});
    EXPECT_FALSE(r.success);
    // 确认什么都没写进 memory 目录(除了可能已存在的 MEMORY.md)
    int entry_count = 0;
    for (auto& de : fs::directory_iterator(acecode::get_memory_dir())) {
        if (de.path().stem() != "MEMORY") ++entry_count;
    }
    EXPECT_EQ(entry_count, 0);
}

// 场景:mode=create 遇到已存在 entry 失败
TEST_F(MemoryWriteToolTest, CreateModeConflict) {
    acecode::MemoryRegistry reg;
    reg.scan();
    auto tool = acecode::create_memory_write_tool(reg);

    auto first = tool.execute(
        R"({"name":"dup","type":"user","description":"a","body":"a","mode":"create"})",
        acecode::ToolContext{});
    ASSERT_TRUE(first.success);

    auto second = tool.execute(
        R"({"name":"dup","type":"user","description":"b","body":"b","mode":"create"})",
        acecode::ToolContext{});
    EXPECT_FALSE(second.success);
}

// 场景:mode=update 遇到不存在 entry 失败
TEST_F(MemoryWriteToolTest, UpdateModeMissing) {
    acecode::MemoryRegistry reg;
    reg.scan();
    auto tool = acecode::create_memory_write_tool(reg);
    auto r = tool.execute(
        R"({"name":"ghost","type":"user","description":"x","body":"y","mode":"update"})",
        acecode::ToolContext{});
    EXPECT_FALSE(r.success);
}

// 场景:非法 type 枚举值失败
TEST_F(MemoryWriteToolTest, InvalidTypeFails) {
    acecode::MemoryRegistry reg;
    auto tool = acecode::create_memory_write_tool(reg);
    auto r = tool.execute(
        R"({"name":"foo","type":"notes","description":"x","body":"y"})",
        acecode::ToolContext{});
    EXPECT_FALSE(r.success);
}
