#include <gtest/gtest.h>

#include "config/config.hpp"
#include "session/session_manager.hpp"
#include "tool/worktree_tool.hpp"
#include "utils/utf8_path.hpp"
#include "worktree/worktree_manager.hpp"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace acecode;

namespace {

// EnterWorktree / ExitWorktree 的端到端测试:真实 git 仓库 + 真实
// SessionManager(不落盘 —— 没有消息时 meta 是内存态)+ 捕获式
// switch_session_cwd 回调模拟 AgentLoop 的 cwd 切换。
class WorktreeToolTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!worktree::run_git({"--version"}, "").ok()) {
            GTEST_SKIP() << "git not available on this machine";
        }

        repo_ = fs::temp_directory_path() /
                (std::string("acecode-wt-tool-") +
                 ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::error_code ec;
        fs::remove_all(repo_, ec);
        fs::create_directories(repo_);
        repo_utf8_ = path_to_utf8(repo_);
        ASSERT_TRUE(worktree::run_git({"init", "-b", "main"}, repo_utf8_).ok());
        ASSERT_TRUE(worktree::run_git({"config", "user.email", "t@t"}, repo_utf8_).ok());
        ASSERT_TRUE(worktree::run_git({"config", "user.name", "t"}, repo_utf8_).ok());
        {
            std::ofstream out(repo_ / "README.md", std::ios::binary);
            out << "hi\n";
        }
        ASSERT_TRUE(worktree::run_git({"add", "."}, repo_utf8_).ok());
        ASSERT_TRUE(worktree::run_git({"commit", "-m", "init"}, repo_utf8_).ok());

        sm_.start_session(repo_utf8_, "test-provider", "test-model");
        session_cwd_ = repo_utf8_;

        ctx_.cwd = repo_utf8_;
        ctx_.session_manager = &sm_;
        ctx_.switch_session_cwd = [this](const std::string& new_cwd) {
            session_cwd_ = new_cwd;
            ctx_.cwd = new_cwd;
        };

        enter_ = create_enter_worktree_tool(WorktreeConfig{});
        exit_ = create_exit_worktree_tool();
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(repo_, ec);
    }

    fs::path repo_;
    std::string repo_utf8_;
    std::string session_cwd_;
    SessionManager sm_;
    ToolContext ctx_;
    ToolImpl enter_;
    ToolImpl exit_;
};

} // namespace

// 场景:用户明确要求进 worktree,EnterWorktree(name="feat")。
// 期望:worktree 建好、会话 cwd 切进去、SessionManager 记录了完整的
// WorktreeSessionInfo(exit 的变更基线依赖 original_head_commit)。
// 再次 EnterWorktree 时报错 —— 一个会话同一时刻只允许一个 worktree。
TEST_F(WorktreeToolTest, EnterCreatesWorktreeAndSwitchesSession) {
    auto result = enter_.execute(R"({"name":"feat"})", ctx_);
    ASSERT_TRUE(result.success) << result.output;

    const auto info = sm_.active_worktree();
    EXPECT_TRUE(info.active());
    EXPECT_EQ(info.worktree_name, "feat");
    EXPECT_EQ(info.worktree_branch, "worktree-feat");
    EXPECT_EQ(info.original_cwd, repo_utf8_);
    EXPECT_FALSE(info.original_head_commit.empty());
    EXPECT_EQ(session_cwd_, info.worktree_path);
    EXPECT_TRUE(fs::exists(path_from_utf8(info.worktree_path) / "README.md"));

    auto again = enter_.execute(R"({"name":"other"})", ctx_);
    EXPECT_FALSE(again.success);
    EXPECT_NE(again.output.find("Already in a worktree session"), std::string::npos);
}

// 场景:非法 slug(路径穿越 "../escape")。
// 期望:创建前置校验直接拒绝,不产生任何 git 副作用。
TEST_F(WorktreeToolTest, EnterRejectsInvalidSlug) {
    auto result = enter_.execute(R"({"name":"../escape"})", ctx_);
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(sm_.active_worktree().active());
    EXPECT_EQ(session_cwd_, repo_utf8_);
}

// 场景:没有活动 worktree 会话时调 ExitWorktree。
// 期望:no-op 报错(与 Claude Code 一致的说明文案),明确"手工建的
// worktree 不归它管、磁盘无改动"。
TEST_F(WorktreeToolTest, ExitWithoutSessionIsNoOp) {
    auto result = exit_.execute(R"({"action":"keep"})", ctx_);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.output.find("No-op"), std::string::npos);
}

// 场景:worktree 里有未提交文件,ExitWorktree(remove) 未带 discard_changes。
// 期望:fail-closed 拒绝并说明有几个未提交文件;会话仍在 worktree 里。
// 带 discard_changes=true 重试 → 删除成功、目录消失、cwd 回原目录。
// 回归背景:这是防止模型误删用户未提交工作的核心安全门。
TEST_F(WorktreeToolTest, RemoveRefusesDirtyWorktreeUntilDiscardConfirmed) {
    ASSERT_TRUE(enter_.execute(R"({"name":"dirty"})", ctx_).success);
    const auto info = sm_.active_worktree();
    {
        std::ofstream out(path_from_utf8(info.worktree_path) / "wip.txt",
                          std::ios::binary);
        out << "uncommitted\n";
    }

    auto refused = exit_.execute(R"({"action":"remove"})", ctx_);
    EXPECT_FALSE(refused.success);
    EXPECT_NE(refused.output.find("1 uncommitted file"), std::string::npos);
    EXPECT_TRUE(sm_.active_worktree().active());
    EXPECT_EQ(session_cwd_, info.worktree_path);

    auto removed = exit_.execute(R"({"action":"remove","discard_changes":true})", ctx_);
    EXPECT_TRUE(removed.success) << removed.output;
    EXPECT_FALSE(sm_.active_worktree().active());
    EXPECT_EQ(session_cwd_, repo_utf8_);
    EXPECT_FALSE(fs::exists(path_from_utf8(info.worktree_path)));
}

// 场景:ExitWorktree(keep)。
// 期望:会话回到原目录、状态清空,但 worktree 目录与分支原样保留 ——
// 用户之后可以手动回去继续,或用同名 EnterWorktree fast-resume。
TEST_F(WorktreeToolTest, KeepPreservesWorktreeOnDisk) {
    ASSERT_TRUE(enter_.execute(R"({"name":"keepme"})", ctx_).success);
    const auto info = sm_.active_worktree();

    auto kept = exit_.execute(R"({"action":"keep"})", ctx_);
    EXPECT_TRUE(kept.success) << kept.output;
    EXPECT_FALSE(sm_.active_worktree().active());
    EXPECT_EQ(session_cwd_, repo_utf8_);
    EXPECT_TRUE(fs::exists(path_from_utf8(info.worktree_path)));

    // fast-resume:同名再进,existed 路径复用同一目录
    auto again = enter_.execute(R"({"name":"keepme"})", ctx_);
    EXPECT_TRUE(again.success) << again.output;
    EXPECT_NE(again.output.find("Resumed existing worktree"), std::string::npos);
    EXPECT_EQ(sm_.active_worktree().worktree_path, info.worktree_path);
}
