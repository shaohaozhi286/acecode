// 覆盖 src/provider/cwd_model_override.{hpp,cpp} 的读写。对应
// openspec/changes/model-profiles 的任务 7.15-7.20。
// 文件头与每个 TEST 都加中文注释,遵循 feedback_unit_test_chinese_comments 约定。
//
// 注意:cwd_model_override 内部用 SessionStorage::get_project_dir(cwd),
// 该函数把 ~/.acecode/projects/<hash> 解析为绝对路径(读 USERPROFILE/HOME)。
// 这里用一个真实存在的临时目录作 cwd,在 HOME 下生成对应 <hash>;测试结束
// 时把生成的 model_override.json 删掉,避免污染开发者环境。

#include <gtest/gtest.h>

#include "provider/cwd_model_override.hpp"
#include "session/session_storage.hpp"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace fs = std::filesystem;
using namespace acecode;

namespace {

// 为本测试制造一个唯一的 cwd —— 用 temp_directory + 随机后缀,确保不同测试
// 之间的 model_override 文件互不干扰。返回该 cwd 路径(已 create)。
fs::path make_unique_cwd() {
    auto tmp = fs::temp_directory_path() / ("acecode_cwdtest_" +
        std::to_string(std::random_device{}()));
    fs::create_directories(tmp);
    return tmp;
}

// 测试结束清理 —— 删本次写出的 model_override 文件 + 其父目录(如果只剩这个)。
void cleanup(const fs::path& cwd) {
    std::string p = cwd_model_override_path(cwd);
    std::error_code ec;
    fs::remove(p, ec);
    fs::remove(p + ".tmp", ec);
    // 不删 cwd —— 是 system temp 目录的子路径,留给系统清理。
}

} // namespace

// 7.16 — 文件不存在 → load 返回 nullopt。
TEST(CwdModelOverrideTest, LoadReturnsNulloptWhenMissing) {
    fs::path cwd = make_unique_cwd();
    cleanup(cwd);  // 确保起点没有文件

    auto got = load_cwd_model_override(cwd);
    EXPECT_FALSE(got.has_value());

    cleanup(cwd);
}

// 7.17 — 文件 malformed JSON → load 返回 nullopt,不抛。
TEST(CwdModelOverrideTest, LoadHandlesMalformedJson) {
    fs::path cwd = make_unique_cwd();
    std::string p = cwd_model_override_path(cwd);
    fs::create_directories(fs::path(p).parent_path());
    {
        std::ofstream ofs(p);
        ofs << "{ this is not json";
    }

    EXPECT_NO_THROW({
        auto got = load_cwd_model_override(cwd);
        EXPECT_FALSE(got.has_value());
    });

    cleanup(cwd);
}

// 7.18 — 合法文件 `{"model_name": "x"}` → load 返回 "x"。
TEST(CwdModelOverrideTest, LoadReadsValidFile) {
    fs::path cwd = make_unique_cwd();
    std::string p = cwd_model_override_path(cwd);
    fs::create_directories(fs::path(p).parent_path());
    {
        std::ofstream ofs(p);
        ofs << R"({"model_name": "my-claude"})";
    }

    auto got = load_cwd_model_override(cwd);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, "my-claude");

    cleanup(cwd);
}

// 7.19 — save 后 load 可 round-trip(同 name 出来,不变形)。
TEST(CwdModelOverrideTest, SaveLoadRoundTrip) {
    fs::path cwd = make_unique_cwd();
    cleanup(cwd);

    save_cwd_model_override(cwd, "round-trip-name");
    auto got = load_cwd_model_override(cwd);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, "round-trip-name");

    cleanup(cwd);
}

// 7.20 — save 原子性:写完后最终文件存在,tmp 文件 MUST NOT 残留。
TEST(CwdModelOverrideTest, SaveLeavesNoTempArtifact) {
    fs::path cwd = make_unique_cwd();
    cleanup(cwd);

    save_cwd_model_override(cwd, "atomic-name");
    std::string final_path = cwd_model_override_path(cwd);
    std::string tmp_path = final_path + ".tmp";

    EXPECT_TRUE(fs::exists(final_path));
    EXPECT_FALSE(fs::exists(tmp_path));

    cleanup(cwd);
}

// 额外 — remove 删掉文件后 load 应返回 nullopt;再 remove 不抛。
TEST(CwdModelOverrideTest, RemoveDeletesFile) {
    fs::path cwd = make_unique_cwd();
    save_cwd_model_override(cwd, "to-be-removed");
    ASSERT_TRUE(load_cwd_model_override(cwd).has_value());

    remove_cwd_model_override(cwd);
    EXPECT_FALSE(load_cwd_model_override(cwd).has_value());

    // 二次 remove 不应抛
    EXPECT_NO_THROW(remove_cwd_model_override(cwd));

    cleanup(cwd);
}
