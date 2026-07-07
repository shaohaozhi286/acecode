#include <gtest/gtest.h>

#include "worktree/worktree_core.hpp"
#include "worktree/worktree_manager.hpp"
#include "utils/utf8_path.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace acecode;
using namespace acecode::worktree;

namespace {

// 每个用例一个独立临时 git 仓库(git init + 一次提交)。机器上没有可用的
// git 时整个 fixture GTEST_SKIP —— 这些是真实 git 集成测试,不做 mock。
class WorktreeGitTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto probe = run_git({"--version"}, "");
        if (!probe.ok()) {
            GTEST_SKIP() << "git not available on this machine";
        }

        repo_ = fs::temp_directory_path() /
                ("acecode-wt-test-" + std::to_string(::testing::UnitTest::GetInstance()
                                                          ->random_seed()) +
                 "-" + ::testing::UnitTest::GetInstance()
                           ->current_test_info()
                           ->name());
        // 上一次运行 TearDown 失败(Windows 文件锁)时会留残骸,先清一次,
        // 否则残留的 .acecode/worktrees 会让计数断言漂移。
        std::error_code ec;
        fs::remove_all(repo_, ec);
        fs::create_directories(repo_);
        repo_utf8_ = path_to_utf8(repo_);

        ASSERT_TRUE(run_git({"init", "-b", "main"}, repo_utf8_).ok());
        // 显式配置提交身份:CI 环境没有全局 user.name/user.email 时
        // commit 会失败,测试不应依赖机器全局配置。
        ASSERT_TRUE(run_git({"config", "user.email", "test@acecode.local"},
                            repo_utf8_).ok());
        ASSERT_TRUE(run_git({"config", "user.name", "acecode-test"}, repo_utf8_).ok());
        write_file(repo_ / "README.md", "hello\n");
        ASSERT_TRUE(run_git({"add", "."}, repo_utf8_).ok());
        ASSERT_TRUE(run_git({"commit", "-m", "init"}, repo_utf8_).ok());
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(repo_, ec);  // best-effort;残留只是临时目录垃圾
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

// 场景:从仓库根目录 / 子目录定位 git root。
// 期望:两者都解析到仓库根;非仓库目录返回 ""。
TEST_F(WorktreeGitTest, FindsGitRootFromSubdirectory) {
    fs::create_directories(repo_ / "src" / "deep");
    const std::string from_root = find_git_root(repo_utf8_);
    const std::string from_sub = find_git_root(path_to_utf8(repo_ / "src" / "deep"));
    EXPECT_FALSE(from_root.empty());
    EXPECT_EQ(from_root, from_sub);

    const fs::path outside = fs::temp_directory_path();
    // temp 目录本身可能意外落在某个仓库里(开发机),只断言不等于本仓库
    EXPECT_NE(find_git_root(path_to_utf8(outside)), from_root);
}

// 场景:首次创建 worktree,然后同名再次调用。
// 期望:首次 existed=false,目录落在 <repo>/.acecode/worktrees/<slug>,
// 分支为 worktree-<slug>,基线 SHA 非空;二次调用走 fast-resume
// (existed=true,不重复创建、不跑 fetch)。
TEST_F(WorktreeGitTest, CreatesThenResumesWorktree) {
    auto first = get_or_create_worktree(repo_utf8_, "feat-a");
    ASSERT_TRUE(first.ok) << first.error;
    EXPECT_FALSE(first.existed);
    EXPECT_EQ(first.worktree_branch, "worktree-feat-a");
    EXPECT_FALSE(first.head_commit.empty());
    EXPECT_TRUE(fs::exists(path_from_utf8(first.worktree_path) / "README.md"));
    EXPECT_TRUE(fs::exists(repo_ / ".acecode" / "worktrees" / "feat-a"));

    auto second = get_or_create_worktree(repo_utf8_, "feat-a");
    ASSERT_TRUE(second.ok) << second.error;
    EXPECT_TRUE(second.existed);
    EXPECT_EQ(second.worktree_path, first.worktree_path);
    EXPECT_FALSE(second.head_commit.empty());
}

// 场景:在 linked worktree 内部调用 find_canonical_git_root。
// 期望:穿透到主仓根,而不是 worktree 自己的根 —— EnterWorktree 在已处于
// worktree 中时必须把新 worktree 建在主仓的 .acecode/worktrees/ 下,
// 否则嵌套目录会被父级 remove 连带删除。
TEST_F(WorktreeGitTest, CanonicalRootResolvesThroughWorktree) {
    auto created = get_or_create_worktree(repo_utf8_, "canon");
    ASSERT_TRUE(created.ok) << created.error;

    const std::string canonical = find_canonical_git_root(created.worktree_path);
    const std::string main_root = find_canonical_git_root(repo_utf8_);
    EXPECT_EQ(fs::weakly_canonical(path_from_utf8(canonical)),
              fs::weakly_canonical(path_from_utf8(main_root)));
    EXPECT_EQ(fs::weakly_canonical(path_from_utf8(canonical)),
              fs::weakly_canonical(repo_));
}

// 场景:干净 worktree / 有未提交文件 / 有新提交 三种状态的变更计数。
// 期望:0/0 → changed_files 计数 → commits 计数。这是 ExitWorktree
// remove 的安全门数据源。
TEST_F(WorktreeGitTest, CountsChangesAndCommits) {
    auto created = get_or_create_worktree(repo_utf8_, "count");
    ASSERT_TRUE(created.ok) << created.error;

    auto clean = count_worktree_changes(created.worktree_path, created.head_commit);
    ASSERT_TRUE(clean.has_value());
    EXPECT_EQ(clean->changed_files, 0);
    EXPECT_EQ(clean->commits, 0);

    write_file(path_from_utf8(created.worktree_path) / "new.txt", "x\n");
    auto dirty = count_worktree_changes(created.worktree_path, created.head_commit);
    ASSERT_TRUE(dirty.has_value());
    EXPECT_EQ(dirty->changed_files, 1);
    EXPECT_EQ(dirty->commits, 0);

    ASSERT_TRUE(run_git({"add", "."}, created.worktree_path).ok());
    ASSERT_TRUE(run_git({"-c", "user.email=t@t", "-c", "user.name=t",
                         "commit", "-m", "wt"},
                        created.worktree_path).ok());
    auto committed = count_worktree_changes(created.worktree_path, created.head_commit);
    ASSERT_TRUE(committed.has_value());
    EXPECT_EQ(committed->changed_files, 0);
    EXPECT_EQ(committed->commits, 1);
}

// 场景:基线 SHA 缺失(hook 包 git 的路径 / 老 meta 数据损坏)。
// 期望:返回 nullopt(fail-closed)而不是谎报 0/0 —— 调用方把 nullopt
// 当"有变更"处理,静默 0/0 会让 remove 毁掉真实工作。
TEST_F(WorktreeGitTest, MissingBaselineFailsClosed) {
    auto created = get_or_create_worktree(repo_utf8_, "baseline");
    ASSERT_TRUE(created.ok) << created.error;
    EXPECT_FALSE(count_worktree_changes(created.worktree_path, "").has_value());
}

// 场景:remove_worktree 删除 worktree 目录与临时分支。
// 期望:目录消失、分支消失;list_worktree_paths 不再包含它。
TEST_F(WorktreeGitTest, RemovesWorktreeAndBranch) {
    auto created = get_or_create_worktree(repo_utf8_, "gone");
    ASSERT_TRUE(created.ok) << created.error;
    ASSERT_TRUE(fs::exists(path_from_utf8(created.worktree_path)));

    const auto before = list_worktree_paths(repo_utf8_);
    EXPECT_EQ(before.size(), 2u);  // 主仓 + 新 worktree

    std::string error;
    ASSERT_TRUE(remove_worktree(repo_utf8_, created.worktree_path,
                                created.worktree_branch, &error))
        << error;
    EXPECT_FALSE(fs::exists(path_from_utf8(created.worktree_path)));
    // 分支已删:show-ref 验证失败
    EXPECT_FALSE(run_git({"show-ref", "--verify", "--quiet",
                          "refs/heads/" + created.worktree_branch},
                         repo_utf8_).ok());
    EXPECT_EQ(list_worktree_paths(repo_utf8_).size(), 1u);
}

// 场景:.worktreeinclude 声明 ".env",仓库里 .env 被 .gitignore 忽略。
// 期望:创建后处理把 .env 拷进 worktree;未声明的 gitignored 文件
// (secret.bin)不拷。回归背景:worktree 是全新 checkout,gitignored 的
// 本地配置(.env / 证书)天然缺失,不拷贝的话进 worktree 后服务起不来。
TEST_F(WorktreeGitTest, CopiesWorktreeIncludeFiles) {
    write_file(repo_ / ".gitignore", ".env\nsecret.bin\n");
    write_file(repo_ / ".worktreeinclude", ".env\n");
    write_file(repo_ / ".env", "TOKEN=abc\n");
    write_file(repo_ / "secret.bin", "raw\n");
    ASSERT_TRUE(run_git({"add", "."}, repo_utf8_).ok());
    ASSERT_TRUE(run_git({"commit", "-m", "ignore"}, repo_utf8_).ok());

    auto created = get_or_create_worktree(repo_utf8_, "inc");
    ASSERT_TRUE(created.ok) << created.error;
    auto copied = copy_worktree_include_files(repo_utf8_, created.worktree_path);

    ASSERT_EQ(copied.size(), 1u);
    EXPECT_EQ(copied[0], ".env");
    EXPECT_TRUE(fs::exists(path_from_utf8(created.worktree_path) / ".env"));
    EXPECT_FALSE(fs::exists(path_from_utf8(created.worktree_path) / "secret.bin"));
}
