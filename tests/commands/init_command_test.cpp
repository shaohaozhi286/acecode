// 覆盖 src/commands/init_command.{hpp,cpp} 的两个纯函数:
// - build_agent_md_skeleton: 离线骨架生成,根据目录下 CLAUDE.md 是否存在拼出迁移提示
// - build_init_prompt: 交给 LLM 的 /init prompt 构建;根据目录下 AGENT.md /
//   CLAUDE.md 存在情况在基础 prompt 末尾选一条 suffix
//
// 完整 /init 命令的 CommandContext 组装依赖 AgentLoop / AppConfig 等运行时对
// 象,不适合单元测试,只在手动端到端验证阶段覆盖

#include <gtest/gtest.h>

#include "commands/init_command.hpp"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

class InitSkeletonTest : public ::testing::Test {
protected:
    fs::path temp_dir;

    void SetUp() override {
        temp_dir = fs::temp_directory_path() /
                   fs::path("acecode-init-skeleton-" +
                            std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
        fs::create_directories(temp_dir);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }

    void touch(const std::string& name) {
        std::ofstream ofs(temp_dir / name);
        ofs << "x\n";
    }
};

// InitPrompt 系列:
// build_init_prompt 产出的字符串是 LLM 直接看到的 user 消息,任何措辞改动都会
// 影响 LLM 的文件生成行为。下面的断言只锁定关键引导句,允许正文表述微调时不破
// 测试,但保证"prefix / 改进 / 迁移"三种分支的意图稳定。
class InitPromptTest : public ::testing::Test {
protected:
    fs::path temp_dir;

    void SetUp() override {
        temp_dir = fs::temp_directory_path() /
                   fs::path("acecode-init-prompt-" +
                            std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
        fs::create_directories(temp_dir);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }

    void touch(const std::string& name) {
        std::ofstream ofs(temp_dir / name);
        ofs << "x\n";
    }
};

} // namespace

// 场景:空目录生成的骨架不含迁移提示,只有四段标题
TEST_F(InitSkeletonTest, EmptyDirHasNoMigrationHint) {
    std::string s = acecode::build_agent_md_skeleton(temp_dir);
    EXPECT_EQ(s.find("acecode already auto-reads"), std::string::npos);
    EXPECT_NE(s.find("# Project Overview"), std::string::npos);
    EXPECT_NE(s.find("# Build Instructions"), std::string::npos);
    EXPECT_NE(s.find("# Architecture"), std::string::npos);
    EXPECT_NE(s.find("# Conventions"), std::string::npos);
}

// 场景:只有 CLAUDE.md 存在时,`mv` 示例命令针对 CLAUDE.md
TEST_F(InitSkeletonTest, ClaudeMdTriggersMigrationHint) {
    touch("CLAUDE.md");
    std::string s = acecode::build_agent_md_skeleton(temp_dir);
    EXPECT_NE(s.find("CLAUDE.md"), std::string::npos);
    EXPECT_NE(s.find("mv CLAUDE.md AGENT.md"), std::string::npos);
}

// 场景:已有 AGENT.md 时,骨架 helper 本身不生成迁移提示(实际 /init 离线路径会拒绝覆盖)
TEST_F(InitSkeletonTest, AgentMdDoesNotTriggerMigrationHint) {
    touch("AGENT.md");
    std::string s = acecode::build_agent_md_skeleton(temp_dir);
    EXPECT_EQ(s.find("mv "), std::string::npos);
}

// 场景:AGENT.md 与 CLAUDE.md 同时存在时,迁移提示仍只针对 legacy CLAUDE.md
TEST_F(InitSkeletonTest, ClaudeMdHintEvenWhenAgentExists) {
    touch("CLAUDE.md");
    touch("AGENT.md");
    std::string s = acecode::build_agent_md_skeleton(temp_dir);
    EXPECT_NE(s.find("CLAUDE.md"), std::string::npos);
    EXPECT_NE(s.find("AGENT.md"), std::string::npos);
    EXPECT_NE(s.find("mv CLAUDE.md AGENT.md"), std::string::npos);
}

// 场景:任何情况下 prompt 都指示 LLM 用 AGENT.md 的前缀块,并且保持
// acecode 指向(不能泄漏 Claude Code 这样的外部产品名)
TEST_F(InitPromptTest, BaseBodyAlwaysIncludesAgentMdPrefix) {
    std::string s = acecode::build_init_prompt(temp_dir);
    EXPECT_NE(s.find("# AGENT.md"), std::string::npos);
    EXPECT_NE(s.find("acecode"), std::string::npos);
    EXPECT_EQ(s.find("Claude Code"), std::string::npos);
    EXPECT_EQ(s.find("CLAUDE.md"), std::string::npos);
}

// 场景:全新目录既不走改进分支也不走迁移分支,基础 prompt 完整但不含任何
// "already exists" / CLAUDE.md 的引导
TEST_F(InitPromptTest, FreshDirectoryHasNoMigrationOrImprovementSuffix) {
    std::string s = acecode::build_init_prompt(temp_dir);
    EXPECT_EQ(s.find("already exists"), std::string::npos);
    EXPECT_EQ(s.find("CLAUDE.md"), std::string::npos);
    // 基础 prompt 的诊断句必须在
    EXPECT_NE(s.find("Please analyze this codebase"), std::string::npos);
}

// 场景:已有 AGENT.md 时触发改进 suffix,让 LLM 用 file_edit_tool 而非
// 覆盖,且明确要求原文已好就留着别改
TEST_F(InitPromptTest, ExistingAgentMdTriggersImprovementSuffix) {
    touch("AGENT.md");
    std::string s = acecode::build_init_prompt(temp_dir);
    EXPECT_NE(s.find("AGENT.md already exists"), std::string::npos);
    EXPECT_NE(s.find("file_edit_tool"), std::string::npos);
    EXPECT_NE(s.find("do not overwrite silently"), std::string::npos);
}

// 场景:只有 CLAUDE.md 时,suffix 点名 CLAUDE.md 作为起点,且不提到
// AGENT.md(否则 LLM 会去找一个不存在的文件)
TEST_F(InitPromptTest, ClaudeMdPresentTriggersMigrationSuffix) {
    touch("CLAUDE.md");
    std::string s = acecode::build_init_prompt(temp_dir);
    EXPECT_NE(s.find("CLAUDE.md already exists"), std::string::npos);
    EXPECT_NE(s.find("file_write_tool"), std::string::npos);
    // 未触发改进分支
    EXPECT_EQ(s.find("AGENT.md already exists"), std::string::npos);
}

// 场景:只有 AGENT.md 时,suffix 点名 AGENT.md,且不提到 CLAUDE.md
TEST_F(InitPromptTest, AgentMdPresentTriggersImprovementSuffix) {
    touch("AGENT.md");
    std::string s = acecode::build_init_prompt(temp_dir);
    EXPECT_NE(s.find("AGENT.md already exists"), std::string::npos);
    EXPECT_NE(s.find("file_edit_tool"), std::string::npos);
    EXPECT_EQ(s.find("CLAUDE.md"), std::string::npos);
}

// 场景:CLAUDE.md / AGENT.md 同时存在时,改进 AGENT.md,CLAUDE.md 作为补充
TEST_F(InitPromptTest, BothLegacyFilesNamedAgentMdPreferred) {
    touch("CLAUDE.md");
    touch("AGENT.md");
    std::string s = acecode::build_init_prompt(temp_dir);
    // 两个文件都必须出现
    EXPECT_NE(s.find("CLAUDE.md"), std::string::npos);
    EXPECT_NE(s.find("AGENT.md"), std::string::npos);
    EXPECT_NE(s.find("AGENT.md already exists"), std::string::npos);
    EXPECT_NE(s.find("file_edit_tool"), std::string::npos);
    // CLAUDE.md 必须是被交叉检查的补充来源
    EXPECT_NE(s.find("cross-check"), std::string::npos);
}

// 场景:同一个目录状态调用两次,结果必须字节一致。这保证了无隐藏随机性
// (例如没有把 time_t 拼进 prompt),测试可复现
TEST_F(InitPromptTest, DeterminismForFixedState) {
    touch("CLAUDE.md");
    std::string a = acecode::build_init_prompt(temp_dir);
    std::string b = acecode::build_init_prompt(temp_dir);
    EXPECT_EQ(a, b);
}
