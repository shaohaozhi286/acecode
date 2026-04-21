// 覆盖 src/memory/memory_registry.{hpp,cpp} 的核心语义:
// - frontmatter 合法/非法 entry 的加载
// - upsert 的 create/update/upsert 三种模式冲突与成功路径
// - 并发读写不会崩溃(加锁)、原子 rename 可见
// - 路径越界/无效 name 被拒绝
//
// 为了不污染用户真实的 ~/.acecode/memory/,在 SetUp 时把 HOME / USERPROFILE
// 重定向到一个 gtest 级别的临时目录,TearDown 时恢复

#include <gtest/gtest.h>

#include "memory/memory_frontmatter.hpp"
#include "memory/memory_index.hpp"
#include "memory/memory_paths.hpp"
#include "memory/memory_registry.hpp"
#include "memory/memory_types.hpp"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
constexpr const char* kHomeEnvName = "USERPROFILE";
#else
constexpr const char* kHomeEnvName = "HOME";
#endif

void set_env(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

std::string read_file(const fs::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

// 每条测试独享一份 HOME,互不干扰
class MemoryRegistryTest : public ::testing::Test {
protected:
    fs::path temp_home;
    std::string prev_home;

    void SetUp() override {
        const char* existing = std::getenv(kHomeEnvName);
        prev_home = existing ? existing : "";

        temp_home = fs::temp_directory_path() /
                    fs::path("acecode-memory-test-" + std::to_string(::testing::UnitTest::GetInstance()->current_test_info()->result()->elapsed_time()) +
                             "-" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::error_code ec;
        fs::remove_all(temp_home, ec);
        fs::create_directories(temp_home);
        set_env(kHomeEnvName, temp_home.string());

        // 预建 memory 目录,简化测试
        fs::create_directories(acecode::get_memory_dir());
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(temp_home, ec);
        set_env(kHomeEnvName, prev_home);
    }

    void write_entry_file(const std::string& name,
                          const std::string& description,
                          const std::string& type,
                          const std::string& body) {
        fs::path p = acecode::get_memory_dir() / (name + ".md");
        std::ofstream ofs(p, std::ios::binary);
        ofs << "---\n"
            << "name: \"" << name << "\"\n"
            << "description: \"" << description << "\"\n"
            << "type: " << type << "\n"
            << "---\n\n"
            << body;
    }
};

} // namespace

// 场景:空目录 scan 后不包含任何 entry
TEST_F(MemoryRegistryTest, EmptyDirScanNoEntries) {
    acecode::MemoryRegistry reg;
    reg.scan();
    EXPECT_EQ(reg.size(), 0u);
}

// 场景:合法 frontmatter 的 entry 能被加载,非法(缺字段/bad type)的被跳过
TEST_F(MemoryRegistryTest, ScanLoadsValidSkipsInvalid) {
    write_entry_file("good", "senior go dev", "user", "10y experience\n");
    write_entry_file("bad_type", "should skip", "notes", "invalid type\n");

    // 故意构造一个缺 type 字段的文件
    {
        std::ofstream ofs(acecode::get_memory_dir() / "bad_missing.md", std::ios::binary);
        ofs << "---\nname: \"bad\"\ndescription: \"no type\"\n---\nbody\n";
    }

    acecode::MemoryRegistry reg;
    reg.scan();
    EXPECT_EQ(reg.size(), 1u);
    auto found = reg.find("good");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->description, "senior go dev");
    EXPECT_EQ(found->type, acecode::MemoryType::User);
}

// 场景:upsert create 模式在新 name 上写入成功,文件 + MEMORY.md 都出现
TEST_F(MemoryRegistryTest, UpsertCreateWritesBothFiles) {
    acecode::MemoryRegistry reg;
    reg.scan();

    std::string err;
    auto e = reg.upsert("go_expert", acecode::MemoryType::User,
                        "10y Go user", "body content",
                        acecode::MemoryWriteMode::Create, err);
    ASSERT_TRUE(e.has_value()) << err;

    // 条目文件实际写入
    fs::path entry_path = acecode::get_memory_dir() / "go_expert.md";
    EXPECT_TRUE(fs::exists(entry_path));
    std::string content = read_file(entry_path);
    EXPECT_NE(content.find("type: user"), std::string::npos);
    EXPECT_NE(content.find("body content"), std::string::npos);

    // MEMORY.md 索引同步包含该条目
    fs::path idx = acecode::get_memory_index_path();
    ASSERT_TRUE(fs::exists(idx));
    std::string idx_text = read_file(idx);
    EXPECT_NE(idx_text.find("go_expert.md"), std::string::npos);
}

// 场景:create 模式遇到已存在的 entry 应失败,不改动任何文件
TEST_F(MemoryRegistryTest, UpsertCreateConflictRejected) {
    write_entry_file("existing", "old desc", "user", "old body\n");
    acecode::MemoryRegistry reg;
    reg.scan();

    std::string err;
    auto e = reg.upsert("existing", acecode::MemoryType::User,
                        "new desc", "new body",
                        acecode::MemoryWriteMode::Create, err);
    EXPECT_FALSE(e.has_value());
    EXPECT_FALSE(err.empty());

    // 原文件仍是 old body
    std::string content = read_file(acecode::get_memory_dir() / "existing.md");
    EXPECT_NE(content.find("old body"), std::string::npos);
}

// 场景:update 模式在 entry 不存在时失败
TEST_F(MemoryRegistryTest, UpsertUpdateMissingRejected) {
    acecode::MemoryRegistry reg;
    reg.scan();

    std::string err;
    auto e = reg.upsert("does_not_exist", acecode::MemoryType::Feedback,
                        "desc", "body",
                        acecode::MemoryWriteMode::Update, err);
    EXPECT_FALSE(e.has_value());
    EXPECT_FALSE(err.empty());
}

// 场景:upsert 模式既支持 create 又支持 update 的通用路径
TEST_F(MemoryRegistryTest, UpsertUpsertMode) {
    acecode::MemoryRegistry reg;
    reg.scan();
    std::string err;

    auto a = reg.upsert("a", acecode::MemoryType::User, "first", "bodyA",
                        acecode::MemoryWriteMode::Upsert, err);
    ASSERT_TRUE(a.has_value()) << err;

    auto b = reg.upsert("a", acecode::MemoryType::User, "updated", "bodyB",
                        acecode::MemoryWriteMode::Upsert, err);
    ASSERT_TRUE(b.has_value()) << err;

    auto found = reg.find("a");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->description, "updated");
}

// 场景:非法 name(路径穿越)被拒绝
TEST_F(MemoryRegistryTest, UpsertRejectsInvalidName) {
    acecode::MemoryRegistry reg;
    reg.scan();
    std::string err;
    auto e = reg.upsert("../evil", acecode::MemoryType::User,
                        "x", "y", acecode::MemoryWriteMode::Create, err);
    EXPECT_FALSE(e.has_value());
    EXPECT_FALSE(err.empty());
}

// 场景:remove 同时删除文件 + MEMORY.md 对应行
TEST_F(MemoryRegistryTest, RemoveDropsFileAndIndexLine) {
    acecode::MemoryRegistry reg;
    reg.scan();
    std::string err;
    auto e = reg.upsert("to_forget", acecode::MemoryType::Project,
                        "temp project", "data",
                        acecode::MemoryWriteMode::Create, err);
    ASSERT_TRUE(e.has_value()) << err;

    ASSERT_TRUE(reg.remove("to_forget", err)) << err;
    EXPECT_FALSE(fs::exists(acecode::get_memory_dir() / "to_forget.md"));
    std::string idx = read_file(acecode::get_memory_index_path());
    EXPECT_EQ(idx.find("to_forget"), std::string::npos);
}

// 场景:并发 upsert 不崩溃,最终状态一致(由 mutex 保证)
TEST_F(MemoryRegistryTest, ConcurrentUpsertIsSafe) {
    acecode::MemoryRegistry reg;
    reg.scan();

    std::atomic<int> ok{0};
    auto worker = [&](int id) {
        std::string err;
        for (int i = 0; i < 10; ++i) {
            std::string name = "t" + std::to_string(id) + "_" + std::to_string(i);
            auto e = reg.upsert(name, acecode::MemoryType::User,
                                "desc " + name, "body " + name,
                                acecode::MemoryWriteMode::Upsert, err);
            if (e.has_value()) ok.fetch_add(1);
        }
    };
    std::thread t1(worker, 1);
    std::thread t2(worker, 2);
    std::thread t3(worker, 3);
    t1.join(); t2.join(); t3.join();

    EXPECT_EQ(ok.load(), 30);
    EXPECT_EQ(reg.size(), 30u);
}

// 场景:read_index_raw 在超过 max_bytes 时截断并加 marker
TEST_F(MemoryRegistryTest, ReadIndexRawTruncates) {
    // 手写一份过大的 MEMORY.md
    std::string big(100, 'x');
    std::ofstream ofs(acecode::get_memory_index_path(), std::ios::binary);
    ofs << big;
    ofs.close();

    acecode::MemoryRegistry reg;
    reg.scan();
    std::string idx = reg.read_index_raw(32);
    EXPECT_GE(idx.size(), 32u);
    EXPECT_NE(idx.find("truncated"), std::string::npos);
}
