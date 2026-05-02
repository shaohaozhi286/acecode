// 覆盖 src/utils/state_file.cpp 的 read_state_flag / write_state_flag。
// 通过 set_state_file_path_for_test 把目标路径切到 GoogleTest 临时目录,避免
// 污染用户真实 ~/.acecode/state.json,也保证测试间相互隔离。
//
// 主要场景:
//   - 文件不存在 → read 返回 false
//   - write 之后 read 拿到对应值
//   - 多个 key 互不覆盖
//   - 损坏的 JSON → read 返回 false,后续 write 成功覆盖
//   - 非对象 JSON(数组 / 标量)同样视为损坏

#include <gtest/gtest.h>

#include "utils/state_file.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

class StateFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 在临时目录里给本测试一份独立 state.json 路径。
        auto tmp = fs::temp_directory_path() /
                   ("acecode_state_test_" + std::to_string(std::rand()));
        fs::create_directories(tmp);
        path_ = (tmp / "state.json").string();
        acecode::set_state_file_path_for_test(path_);
    }
    void TearDown() override {
        acecode::set_state_file_path_for_test("");
        std::error_code ec;
        fs::remove_all(fs::path(path_).parent_path(), ec);
    }
    std::string path_;
};

void write_raw(const std::string& path, const std::string& contents) {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    ofs << contents;
}

} // namespace

// 场景:文件不存在 → read 返回 false,不抛异常
TEST_F(StateFileTest, MissingFileReadsFalse) {
    EXPECT_FALSE(fs::exists(path_));
    EXPECT_FALSE(acecode::read_state_flag("legacy_terminal_hint_shown"));
}

// 场景:write 之后 read 立刻看到 true
TEST_F(StateFileTest, WriteThenReadTrue) {
    acecode::write_state_flag("legacy_terminal_hint_shown", true);
    EXPECT_TRUE(fs::exists(path_));
    EXPECT_TRUE(acecode::read_state_flag("legacy_terminal_hint_shown"));
}

// 场景:write false 也写入(显式 reset 用)
TEST_F(StateFileTest, WriteFalseExplicitlyStored) {
    acecode::write_state_flag("legacy_terminal_hint_shown", true);
    acecode::write_state_flag("legacy_terminal_hint_shown", false);
    EXPECT_FALSE(acecode::read_state_flag("legacy_terminal_hint_shown"));
}

// 场景:写入新 key 不应覆盖已有 key
TEST_F(StateFileTest, MultipleKeysCoexist) {
    acecode::write_state_flag("legacy_terminal_hint_shown", true);
    acecode::write_state_flag("another_flag", true);
    EXPECT_TRUE(acecode::read_state_flag("legacy_terminal_hint_shown"));
    EXPECT_TRUE(acecode::read_state_flag("another_flag"));
}

// 场景:已存在但内容是非合法 JSON → read 视同 false,后续 write 覆盖成功
TEST_F(StateFileTest, CorruptedJsonReadsFalseAndWriteOverwrites) {
    write_raw(path_, "this is not json {{{");
    EXPECT_FALSE(acecode::read_state_flag("legacy_terminal_hint_shown"));

    acecode::write_state_flag("legacy_terminal_hint_shown", true);
    EXPECT_TRUE(acecode::read_state_flag("legacy_terminal_hint_shown"));
}

// 场景:JSON 合法但顶层是数组(不是对象)→ 视同损坏,read=false
TEST_F(StateFileTest, NonObjectJsonTreatedAsCorrupted) {
    write_raw(path_, "[1, 2, 3]");
    EXPECT_FALSE(acecode::read_state_flag("legacy_terminal_hint_shown"));
}

// 场景:key 存在但 value 不是 bool(比如字符串)→ read=false
TEST_F(StateFileTest, NonBoolValueReadsFalse) {
    write_raw(path_, R"({"legacy_terminal_hint_shown": "yes"})");
    EXPECT_FALSE(acecode::read_state_flag("legacy_terminal_hint_shown"));
}

// 场景:文件为空 → read=false,write 之后正常工作
TEST_F(StateFileTest, EmptyFileTreatedAsEmptyState) {
    write_raw(path_, "");
    EXPECT_FALSE(acecode::read_state_flag("legacy_terminal_hint_shown"));
    acecode::write_state_flag("legacy_terminal_hint_shown", true);
    EXPECT_TRUE(acecode::read_state_flag("legacy_terminal_hint_shown"));
}

// 场景:last_active_workspace_hash 序列化往返 — desktop 多 workspace 模型靠这条
// 跨启动持久化"上次活跃 workspace"。
TEST_F(StateFileTest, LastActiveWorkspaceHashRoundTrip) {
    EXPECT_EQ(acecode::read_last_active_workspace_hash(), ""); // 初始空
    acecode::write_last_active_workspace_hash("abc1234567890def");
    EXPECT_EQ(acecode::read_last_active_workspace_hash(), "abc1234567890def");
    // 覆盖写
    acecode::write_last_active_workspace_hash("ffffffffffffffff");
    EXPECT_EQ(acecode::read_last_active_workspace_hash(), "ffffffffffffffff");
    // 共存其他 key 不互相覆盖
    acecode::write_state_flag("some_flag", true);
    EXPECT_EQ(acecode::read_last_active_workspace_hash(), "ffffffffffffffff");
    EXPECT_TRUE(acecode::read_state_flag("some_flag"));
}

// 场景:last_active_workspace_hash 字段类型不对(数字)→ read 返回空字符串而不是抛
TEST_F(StateFileTest, LastActiveWrongTypeReadsEmpty) {
    write_raw(path_, R"({"last_active_workspace_hash": 12345})");
    EXPECT_EQ(acecode::read_last_active_workspace_hash(), "");
}
