#include <gtest/gtest.h>

#include "prompt/system_prompt.hpp"

#include <string>

// gitStatus 快照的 prompt 层测试(openspec add-git-context)。
// 采集本身在 tests/gitinfo 覆盖;这里只测包装与 session-context 拼装。

// 场景:快照文本非空。
// 期望:包成 "# Git Status" 块,cache_key 非空且随内容变化。
TEST(GitStatusPromptTest, WrapsSnapshotIntoBlock) {
    auto block = acecode::build_git_status_context_prompt(
        "Current branch: main\n\nStatus:\n(clean)");
    ASSERT_FALSE(block.content.empty());
    EXPECT_NE(block.content.find("# Git Status"), std::string::npos);
    EXPECT_NE(block.content.find("Current branch: main"), std::string::npos);
    EXPECT_FALSE(block.cache_key.empty());

    auto other = acecode::build_git_status_context_prompt("something else");
    EXPECT_NE(block.cache_key, other.cache_key);
}

// 场景:快照为空(非仓库 / 采集失败 / disabled)。
// 期望:空块 —— 不发送、不出现空标题。
TEST(GitStatusPromptTest, EmptySnapshotYieldsEmptyBlock) {
    auto block = acecode::build_git_status_context_prompt("");
    EXPECT_TRUE(block.content.empty());
    EXPECT_TRUE(block.cache_key.empty());
}

// 场景:session context 只有 git 快照一个成员(无项目指令/memory/skills)。
// 期望:整个 system-reminder 仍然生成(git 快照独立成块,不依赖其它成员),
// 且内容包含快照;快照为空时回到"什么都没有 → 空块"的旧行为。
TEST(GitStatusPromptTest, SessionContextCarriesGitSnapshot) {
    auto with_git = acecode::build_session_context_prompt(
        /*cwd=*/"", nullptr, nullptr, nullptr, nullptr, 0, nullptr,
        "Current branch: main");
    ASSERT_FALSE(with_git.content.empty());
    EXPECT_NE(with_git.content.find("# Git Status"), std::string::npos);
    EXPECT_NE(with_git.content.find("<system-reminder>"), std::string::npos);

    auto without_git = acecode::build_session_context_prompt(
        /*cwd=*/"", nullptr, nullptr, nullptr, nullptr, 0, nullptr, "");
    EXPECT_TRUE(without_git.content.empty());
}

// 场景:同一快照文本重复构建 session context(会话内多个 turn)。
// 期望:cache_key 稳定 —— AgentLoop 的 cached_context_for_api 依赖 key
// 不漂移来避免重复注入差异内容。
TEST(GitStatusPromptTest, SessionContextCacheKeyStable) {
    auto a = acecode::build_session_context_prompt(
        "", nullptr, nullptr, nullptr, nullptr, 0, nullptr, "snap");
    auto b = acecode::build_session_context_prompt(
        "", nullptr, nullptr, nullptr, nullptr, 0, nullptr, "snap");
    EXPECT_EQ(a.cache_key, b.cache_key);

    auto c = acecode::build_session_context_prompt(
        "", nullptr, nullptr, nullptr, nullptr, 0, nullptr, "snap2");
    EXPECT_NE(a.cache_key, c.cache_key);
}
