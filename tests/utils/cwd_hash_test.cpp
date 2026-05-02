// 覆盖 src/utils/cwd_hash.cpp。这层算法是 desktop WorkspaceRegistry 与 daemon
// SessionStorage 共享的 — 一旦行为漂移,两边对同一 cwd 算出不同目录,desktop
// 找不到 daemon 写入的 sessions。所有规范化分支(反斜杠/大小写/尾斜杠)都要锁住。

#include <gtest/gtest.h>

#include "utils/cwd_hash.hpp"
#include "session/session_storage.hpp"

using acecode::compute_cwd_hash;
using acecode::SessionStorage;

// 场景: 反斜杠路径 == 正斜杠路径(Windows / Unix 表达式互通)
TEST(CwdHash, BackslashEqualsForwardSlash) {
    EXPECT_EQ(compute_cwd_hash("N:\\Users\\shao\\acecode"),
              compute_cwd_hash("N:/Users/shao/acecode"));
}

// 场景: 大小写不敏感(Windows 大小写不敏感的文件系统兜底,macOS HFS+ 默认也是)
TEST(CwdHash, CaseInsensitive) {
    EXPECT_EQ(compute_cwd_hash("N:/Users/Shao/AceCode"),
              compute_cwd_hash("n:/users/shao/acecode"));
}

// 场景: 尾斜杠不影响 hash("foo/" 与 "foo" 算同一目录)
TEST(CwdHash, TrailingSlashIgnored) {
    EXPECT_EQ(compute_cwd_hash("/home/user/proj"),
              compute_cwd_hash("/home/user/proj/"));
}

// 场景: 不同路径产出不同 hash(避免退化到常量)
TEST(CwdHash, DistinctPathsDistinctHash) {
    EXPECT_NE(compute_cwd_hash("/home/user/a"),
              compute_cwd_hash("/home/user/b"));
}

// 场景: hash 长度恒定 16 位十六进制(目录名约定)
TEST(CwdHash, OutputLengthIs16Hex) {
    EXPECT_EQ(compute_cwd_hash("/x").size(), 16u);
    EXPECT_EQ(compute_cwd_hash("").size(), 16u);
}

// 场景: 空字符串与单字符的兜底输出仍是合法 16 位 hex
TEST(CwdHash, EdgeCasesEmptyAndShort) {
    auto h_empty = compute_cwd_hash("");
    auto h_short = compute_cwd_hash("/");
    EXPECT_EQ(h_empty.size(), 16u);
    EXPECT_EQ(h_short.size(), 16u);
    // 单 "/" 经过去尾斜杠会保留一个 "/"(去尾保护规则:size > 1 才去),
    // 与空字符串自然不同 hash。
    EXPECT_NE(h_empty, h_short);
}

// 场景: SessionStorage 与 desktop 路径必须算出**完全相同** hash(关键不变量)。
// 任何一边将来重构都不能让此测试退化。
TEST(CwdHash, SharedWithSessionStorage) {
    std::string cwd = "N:/Users/shao/acecode";
    EXPECT_EQ(compute_cwd_hash(cwd), SessionStorage::compute_project_hash(cwd));
}
