#include <gtest/gtest.h>

#include "session/session_storage.hpp"
#include "utils/utf8_path.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using namespace acecode;

namespace {

fs::path temp_meta_path(const char* name) {
    return fs::temp_directory_path() / (std::string("acecode-wt-meta-") + name + ".json");
}

std::string read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream buf;
    buf << in.rdbuf();
    return buf.str();
}

} // namespace

// 场景:会话进入 worktree 后写 meta,再读回。
// 期望:WorktreeSessionInfo 五个字段完整往返 —— resume 依赖这份数据
// 把会话 cwd 切回 worktree 并让 ExitWorktree 能算变更基线。
TEST(WorktreeMetaRoundtrip, PersistsAndRestoresWorktreeSession) {
    const fs::path meta_file = temp_meta_path("roundtrip");

    SessionMeta meta;
    meta.id = "20260708-000000-abcd";
    meta.cwd = "C:/repo";
    meta.worktree.original_cwd = "C:/repo";
    meta.worktree.worktree_path = "C:/repo/.acecode/worktrees/feat";
    meta.worktree.worktree_name = "feat";
    meta.worktree.worktree_branch = "worktree-feat";
    meta.worktree.original_head_commit = "abc123";
    SessionStorage::write_meta(path_to_utf8(meta_file), meta);

    auto loaded = SessionStorage::read_meta(path_to_utf8(meta_file));
    EXPECT_TRUE(loaded.worktree.active());
    EXPECT_EQ(loaded.worktree.original_cwd, "C:/repo");
    EXPECT_EQ(loaded.worktree.worktree_path, "C:/repo/.acecode/worktrees/feat");
    EXPECT_EQ(loaded.worktree.worktree_name, "feat");
    EXPECT_EQ(loaded.worktree.worktree_branch, "worktree-feat");
    EXPECT_EQ(loaded.worktree.original_head_commit, "abc123");

    std::error_code ec;
    fs::remove(meta_file, ec);
}

// 场景:普通会话(没进过 worktree)写 meta。
// 期望:meta JSON 里完全没有 worktree_session 字段 —— 与 forked_from 等
// 可选字段一致的"空则省略"约定,保证老 meta 文件 byte-byte 不变;
// 读回后 active() 为 false。
TEST(WorktreeMetaRoundtrip, InactiveWorktreeIsOmittedFromJson) {
    const fs::path meta_file = temp_meta_path("inactive");

    SessionMeta meta;
    meta.id = "20260708-000001-abcd";
    meta.cwd = "C:/repo";
    SessionStorage::write_meta(path_to_utf8(meta_file), meta);

    EXPECT_EQ(read_all(meta_file).find("worktree_session"), std::string::npos);
    auto loaded = SessionStorage::read_meta(path_to_utf8(meta_file));
    EXPECT_FALSE(loaded.worktree.active());

    std::error_code ec;
    fs::remove(meta_file, ec);
}
