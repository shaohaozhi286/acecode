#include <gtest/gtest.h>

#include "gitinfo/git_context_collector.hpp"
#include "web/handlers/git_handler.hpp"
#include "worktree/worktree_manager.hpp"
#include "utils/utf8_path.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace acecode;
using namespace acecode::gitinfo;

namespace {

// 每个用例一个独立临时 git 仓库(真实 git 集成,不 mock;机器无 git 时
// GTEST_SKIP)—— 与 tests/worktree 的 fixture 同款做法。
class GitContextCollectorTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto probe = worktree::run_git({"--version"}, "");
        if (!probe.ok()) {
            GTEST_SKIP() << "git not available on this machine";
        }

        repo_ = fs::temp_directory_path() /
                ("acecode-gitctx-test-" +
                 std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                 "-" + ::testing::UnitTest::GetInstance()
                           ->current_test_info()
                           ->name());
        std::error_code ec;
        fs::remove_all(repo_, ec);
        fs::create_directories(repo_);
        repo_utf8_ = path_to_utf8(repo_);

        ASSERT_TRUE(worktree::run_git({"init", "-b", "main"}, repo_utf8_).ok());
        ASSERT_TRUE(worktree::run_git({"config", "user.email", "t@acecode.local"},
                                      repo_utf8_).ok());
        ASSERT_TRUE(worktree::run_git({"config", "user.name", "acecode-test"},
                                      repo_utf8_).ok());
        write_file(repo_ / "README.md", "hello\n");
        ASSERT_TRUE(worktree::run_git({"add", "."}, repo_utf8_).ok());
        ASSERT_TRUE(worktree::run_git({"commit", "-m", "init commit"},
                                      repo_utf8_).ok());
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(repo_, ec);
    }

    static void write_file(const fs::path& p, const std::string& content) {
        fs::create_directories(p.parent_path());
        std::ofstream out(p, std::ios::binary);
        out << content;
    }

    fs::path repo_;
    std::string repo_utf8_;
};

} // namespace

// ---- collect_git_status_snapshot -------------------------------------------

// 场景:干净仓库采集快照。
// 期望:包含 snapshot 声明、分支 main、"(clean)" status、最近提交行、
// 仓库级 user.name。
TEST_F(GitContextCollectorTest, SnapshotOnCleanRepo) {
    std::string snap = collect_git_status_snapshot(repo_utf8_);
    ASSERT_FALSE(snap.empty());
    EXPECT_NE(snap.find("snapshot in time"), std::string::npos);
    EXPECT_NE(snap.find("Current branch: main"), std::string::npos);
    EXPECT_NE(snap.find("Status:\n(clean)"), std::string::npos);
    EXPECT_NE(snap.find("init commit"), std::string::npos);
    EXPECT_NE(snap.find("Git user: acecode-test"), std::string::npos);
}

// 场景:有未提交改动 + 未跟踪文件时采集快照。
// 期望:status 段包含两者(status --short 的 M / ?? 行)。
TEST_F(GitContextCollectorTest, SnapshotShowsDirtyState) {
    write_file(repo_ / "README.md", "changed\n");
    write_file(repo_ / "new.txt", "x\n");
    std::string snap = collect_git_status_snapshot(repo_utf8_);
    ASSERT_FALSE(snap.empty());
    EXPECT_NE(snap.find("README.md"), std::string::npos);
    EXPECT_NE(snap.find("new.txt"), std::string::npos);
}

// 场景:cwd 不在任何 git 仓库内。
// 期望:返回空串(整块不注入),且不报错。
TEST_F(GitContextCollectorTest, SnapshotEmptyOutsideRepo) {
    fs::path outside = fs::temp_directory_path() / "acecode-gitctx-nonrepo";
    std::error_code ec;
    fs::remove_all(outside, ec);
    fs::create_directories(outside);
    // temp 目录理论上可能落在某个仓库里(开发机);此时跳过而不是误报。
    if (!is_inside_git_repo(path_to_utf8(outside))) {
        EXPECT_TRUE(collect_git_status_snapshot(path_to_utf8(outside)).empty());
    }
    fs::remove_all(outside, ec);
}

// ---- collect_git_info --------------------------------------------------------

// 场景:干净仓库查询 info。fixture 仓库是纯本地 init,没有 origin remote。
// 期望:is_repo=true、branch=main、branches 含 main、dirty=false;
// default_branch 兜底 "main"(prompt 快照要用),但 default_base 必须为
// 空串 —— 修复前它没有这个字段,前端拿 default_branch 拼出不存在的
// origin/main 当基线,变更面板报「加载失败:invalid base」(线上截图)。
TEST_F(GitContextCollectorTest, InfoOnCleanRepo) {
    GitInfo info = collect_git_info(repo_utf8_);
    EXPECT_TRUE(info.is_repo);
    EXPECT_EQ(info.branch, "main");
    ASSERT_FALSE(info.branches.empty());
    EXPECT_NE(std::find(info.branches.begin(), info.branches.end(), "main"),
              info.branches.end());
    EXPECT_FALSE(info.dirty);
    EXPECT_EQ(info.default_branch, "main");
    EXPECT_TRUE(info.default_base.empty());
}

// 场景:仓库存在 origin/main 远程跟踪分支(用 update-ref 直接伪造,免建
// 真实 remote / 免 fetch)。
// 期望:default_base = "origin/main"(经 show-ref 验证存在后才返回)。
TEST_F(GitContextCollectorTest, InfoDefaultBaseWhenOriginRefExists) {
    auto head = worktree::run_git({"rev-parse", "HEAD"}, repo_utf8_);
    ASSERT_TRUE(head.ok());
    std::string sha = head.out;
    while (!sha.empty() && (sha.back() == '\n' || sha.back() == '\r')) {
        sha.pop_back();
    }
    ASSERT_TRUE(worktree::run_git(
        {"update-ref", "refs/remotes/origin/main", sha}, repo_utf8_).ok());

    GitInfo info = collect_git_info(repo_utf8_);
    EXPECT_EQ(info.default_branch, "main");
    EXPECT_EQ(info.default_base, "origin/main");
}

// 场景:tracked 文件被修改(dirty 的定义 = checkout 会被拦的改动);
// 之后再加一个 untracked 文件。
// 期望:tracked 改动 → dirty=true;仅 untracked 不影响 dirty(-uno 语义,
// checkout 不会动 untracked,pill 的 stash 确认门不需要为它弹窗)。
TEST_F(GitContextCollectorTest, InfoDirtyTrackedOnly) {
    write_file(repo_ / "untracked.txt", "x\n");
    GitInfo only_untracked = collect_git_info(repo_utf8_);
    EXPECT_FALSE(only_untracked.dirty);

    write_file(repo_ / "README.md", "changed\n");
    GitInfo tracked_changed = collect_git_info(repo_utf8_);
    EXPECT_TRUE(tracked_changed.dirty);
}

// 场景:多分支仓库。
// 期望:branches 列出全部本地分支;当前分支跟随 checkout。
TEST_F(GitContextCollectorTest, InfoListsBranches) {
    ASSERT_TRUE(worktree::run_git({"branch", "dev"}, repo_utf8_).ok());
    ASSERT_TRUE(worktree::run_git({"checkout", "-b", "feature/x"},
                                  repo_utf8_).ok());
    GitInfo info = collect_git_info(repo_utf8_);
    EXPECT_EQ(info.branch, "feature/x");
    EXPECT_EQ(info.branches.size(), 3u); // main, dev, feature/x
}

// ---- build_git_info_payload(REST handler 纯函数)---------------------------

// 场景:cwd 不在 allowed_cwds 白名单(防 daemon 被当任意路径 git 查询器)。
// 期望:400 unknown workspace,不触达 git。
TEST_F(GitContextCollectorTest, HandlerRejectsUnknownWorkspace) {
    auto resp = web::build_git_info_payload(
        repo_utf8_, {"C:/somewhere/else"}, /*enabled=*/true, 3000);
    EXPECT_EQ(resp.status, 400);
    EXPECT_EQ(resp.body["error"], "unknown workspace");
}

// 场景:git_context.enabled=false(用户一键关闭 git 感知)。
// 期望:200 且 is_repo=false —— 前端自然隐藏 git UI,零 git 子进程。
TEST_F(GitContextCollectorTest, HandlerDisabledReportsNonRepo) {
    auto resp = web::build_git_info_payload(
        repo_utf8_, {repo_utf8_}, /*enabled=*/false, 3000);
    EXPECT_EQ(resp.status, 200);
    EXPECT_FALSE(resp.body["is_repo"].get<bool>());
    EXPECT_FALSE(resp.body.contains("branch"));
}

// 场景:正常仓库 + 白名单命中。
// 期望:200,payload 五字段齐全且与仓库状态一致。
TEST_F(GitContextCollectorTest, HandlerReturnsRepoInfo) {
    auto resp = web::build_git_info_payload(
        repo_utf8_, {repo_utf8_}, /*enabled=*/true, 3000);
    EXPECT_EQ(resp.status, 200);
    EXPECT_TRUE(resp.body["is_repo"].get<bool>());
    EXPECT_EQ(resp.body["branch"], "main");
    EXPECT_FALSE(resp.body["dirty"].get<bool>());
    EXPECT_TRUE(resp.body["branches"].is_array());
}
