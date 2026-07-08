// SessionRegistry::enter_worktree_for_web 的集成测试
// (openspec add-webui-git-session-pill 任务 2.2)。
//
// 覆盖 Web 首条消息 worktree 意图的后端前置步骤:为"尚无消息"的会话创建
// `ses-<id前8>` worktree(基线 = 用户在 pill 选的本地分支)并切会话 cwd。
// 回归表现:pill 勾了 worktree 但消息发出后仍跑在主仓 / 已开聊会话被
// 中途切进 worktree / 不存在的基线被静默换成默认分支。
//
// fixture 做法同 tests/session/session_registry_test.cpp(不跑真 LLM)+
// tests/worktree(真实 git 仓库,无 git 则 SKIP)。

#include <gtest/gtest.h>

#include "permissions.hpp"
#include "config/config.hpp"
#include "session/session_registry.hpp"
#include "tool/tool_executor.hpp"
#include "worktree/worktree_manager.hpp"
#include "utils/utf8_path.hpp"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace fs = std::filesystem;
using namespace acecode;

namespace {

class WebWorktreeIntentTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto probe = worktree::run_git({"--version"}, "");
        if (!probe.ok()) {
            GTEST_SKIP() << "git not available on this machine";
        }

        repo_ = fs::temp_directory_path() /
                ("acecode-webwt-test-" +
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
        ASSERT_TRUE(worktree::run_git({"commit", "-m", "init"}, repo_utf8_).ok());
        ASSERT_TRUE(worktree::run_git({"branch", "dev"}, repo_utf8_).ok());

        SessionRegistryDeps deps;
        deps.provider_accessor = [this] { return provider_; };
        deps.tools = &tools_;
        deps.cwd = repo_utf8_;
        deps.config = &config_;
        deps.template_permissions = &permissions_;
        registry_ = std::make_unique<SessionRegistry>(std::move(deps));
    }

    void TearDown() override {
        registry_.reset(); // 先 join worker,再删临时仓库
        std::error_code ec;
        fs::remove_all(repo_, ec);
    }

    static void write_file(const fs::path& p, const std::string& content) {
        fs::create_directories(p.parent_path());
        std::ofstream out(p, std::ios::binary);
        out << content;
    }

    std::string create_session() {
        SessionOptions opts;
        opts.cwd = repo_utf8_;
        return registry_->create(opts);
    }

    fs::path repo_;
    std::string repo_utf8_;
    ToolExecutor tools_;
    PermissionManager permissions_;
    AppConfig config_;
    std::shared_ptr<LlmProvider> provider_; // nullptr:不跑真 LLM
    std::unique_ptr<SessionRegistry> registry_;
};

} // namespace

// 场景:空会话 + 用户在 pill 选了本地分支 dev 作基线。
// 期望:创建 .acecode/worktrees/ses-<id8>,会话 sm 记录 worktree 状态,
// AgentLoop cwd 切进 worktree,分支为 worktree-ses-<id8>。
TEST_F(WebWorktreeIntentTest, CreatesWorktreeForEmptySession) {
    const std::string id = create_session();
    ASSERT_FALSE(id.empty());

    auto result = registry_->enter_worktree_for_web(id, "dev");
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.http_status, 200);
    EXPECT_TRUE(fs::exists(fs::path(path_from_utf8(result.worktree_path)) / ".git"));
    // slug 用完整 id:前 8 位是日期,同日会话会撞车(回归:第二个会话
    // fast-resume 错误复用第一个会话的 worktree)。
    EXPECT_EQ(result.worktree_branch, "worktree-ses-" + id);

    auto entry = registry_->acquire(id);
    ASSERT_TRUE(entry);
    EXPECT_TRUE(entry->sm->active_worktree().active());
    EXPECT_EQ(entry->sm->active_worktree().worktree_path, result.worktree_path);
    // 会话存储归属(entry->cwd)有意不动 —— worktree 是同项目的临时工作区。
    EXPECT_EQ(entry->cwd, repo_utf8_);
}

// 场景:基线分支不存在 / 分支名带注入字符。
// 期望:4xx 拒绝且不创建任何 worktree 目录(用户点名的基线绝不被静默换掉)。
TEST_F(WebWorktreeIntentTest, RejectsInvalidBase) {
    const std::string id = create_session();

    auto missing = registry_->enter_worktree_for_web(id, "no-such-branch");
    EXPECT_FALSE(missing.ok) << "path=" << missing.worktree_path
                             << " branch=" << missing.worktree_branch
                             << " err=" << missing.error;
    EXPECT_EQ(missing.http_status, 400);

    auto unsafe = registry_->enter_worktree_for_web(id, "$(boom)");
    EXPECT_FALSE(unsafe.ok);
    EXPECT_EQ(unsafe.http_status, 400);

    EXPECT_FALSE(fs::exists(repo_ / ".acecode" / "worktrees" / ("ses-" + id)));
}

// 场景:会话已在 worktree 中再次带意图。
// 期望:400 —— 中途切换归 EnterWorktree 工具,UI 不提供第二条路径。
TEST_F(WebWorktreeIntentTest, RejectsWhenAlreadyInWorktree) {
    const std::string id = create_session();
    ASSERT_TRUE(registry_->enter_worktree_for_web(id, "main").ok);

    auto again = registry_->enter_worktree_for_web(id, "dev");
    EXPECT_FALSE(again.ok);
    EXPECT_EQ(again.http_status, 400);
}

// 场景:未知会话 id。
// 期望:404。
TEST_F(WebWorktreeIntentTest, UnknownSession404) {
    auto result = registry_->enter_worktree_for_web("nope-nope", "main");
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.http_status, 404);
}

// 场景:空基线(pill 未选分支时后端回退默认基线策略)。
// 期望:创建成功(本仓库无 origin,回退当前 HEAD 基线)。
TEST_F(WebWorktreeIntentTest, EmptyBaseFallsBackToDefaultBaseline) {
    const std::string id = create_session();
    auto result = registry_->enter_worktree_for_web(id, "");
    ASSERT_TRUE(result.ok) << result.error;
    auto entry = registry_->acquire(id);
    ASSERT_TRUE(entry);
    EXPECT_TRUE(entry->sm->active_worktree().active());
}
