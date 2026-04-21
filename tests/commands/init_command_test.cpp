// 覆盖 src/commands/init_command.{hpp,cpp} 的两个纯函数:
// - build_acecode_md_skeleton: 离线骨架生成,根据目录下 CLAUDE.md / AGENT.md
//   是否存在拼出迁移提示;由无 provider 回落路径使用
// - build_init_prompt: 交给 LLM 的 /init prompt 构建;根据目录下 ACECODE.md /
//   CLAUDE.md / AGENT.md 存在情况在基础 prompt 末尾选一条 suffix
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
    std::string s = acecode::build_acecode_md_skeleton(temp_dir);
    EXPECT_EQ(s.find("acecode already auto-reads"), std::string::npos);
    EXPECT_NE(s.find("# Project Overview"), std::string::npos);
    EXPECT_NE(s.find("# Build Instructions"), std::string::npos);
    EXPECT_NE(s.find("# Architecture"), std::string::npos);
    EXPECT_NE(s.find("# Conventions"), std::string::npos);
}

// 场景:只有 CLAUDE.md 存在时,`mv` 示例命令针对 CLAUDE.md
TEST_F(InitSkeletonTest, ClaudeMdTriggersMigrationHint) {
    touch("CLAUDE.md");
    std::string s = acecode::build_acecode_md_skeleton(temp_dir);
    EXPECT_NE(s.find("CLAUDE.md"), std::string::npos);
    EXPECT_NE(s.find("mv CLAUDE.md ACECODE.md"), std::string::npos);
    // mv 示例不应引导用户改 AGENT.md(因为它并不存在)
    EXPECT_EQ(s.find("mv AGENT.md"), std::string::npos);
}

// 场景:只有 AGENT.md 存在时,mv 示例指向 AGENT.md
TEST_F(InitSkeletonTest, AgentMdTriggersMigrationHint) {
    touch("AGENT.md");
    std::string s = acecode::build_acecode_md_skeleton(temp_dir);
    EXPECT_NE(s.find("AGENT.md"), std::string::npos);
    EXPECT_NE(s.find("mv AGENT.md ACECODE.md"), std::string::npos);
    // mv 示例不应引导用户改 CLAUDE.md(因为它并不存在)
    EXPECT_EQ(s.find("mv CLAUDE.md"), std::string::npos);
}

// 场景:两种文件同时存在时迁移提示同时列出两者
TEST_F(InitSkeletonTest, BothLegacyFilesListed) {
    touch("CLAUDE.md");
    touch("AGENT.md");
    std::string s = acecode::build_acecode_md_skeleton(temp_dir);
    EXPECT_NE(s.find("CLAUDE.md"), std::string::npos);
    EXPECT_NE(s.find("AGENT.md"), std::string::npos);
    EXPECT_NE(s.find(", "), std::string::npos);
}

// 场景:任何情况下 prompt 都指示 LLM 用 ACECODE.md 的前缀块,并且保持
// acecode 指向(不能泄漏 Claude Code 这样的外部产品名)
TEST_F(InitPromptTest, BaseBodyAlwaysIncludesAcecodeMdPrefix) {
    std::string s = acecode::build_init_prompt(temp_dir);
    EXPECT_NE(s.find("# ACECODE.md"), std::string::npos);
    EXPECT_NE(s.find("acecode"), std::string::npos);
    EXPECT_EQ(s.find("Claude Code"), std::string::npos);
    EXPECT_EQ(s.find("CLAUDE.md"), std::string::npos);
}

// 场景:全新目录既不走改进分支也不走迁移分支,基础 prompt 完整但不含任何
// "already exists" / CLAUDE.md / AGENT.md 的引导
TEST_F(InitPromptTest, FreshDirectoryHasNoMigrationOrImprovementSuffix) {
    std::string s = acecode::build_init_prompt(temp_dir);
    EXPECT_EQ(s.find("already exists"), std::string::npos);
    EXPECT_EQ(s.find("CLAUDE.md"), std::string::npos);
    EXPECT_EQ(s.find("AGENT.md"), std::string::npos);
    // 基础 prompt 的诊断句必须在
    EXPECT_NE(s.find("Please analyze this codebase"), std::string::npos);
}

// 场景:已有 ACECODE.md 时触发改进 suffix,让 LLM 用 file_edit_tool 而非
// 覆盖,且明确要求原文已好就留着别改
TEST_F(InitPromptTest, ExistingAcecodeMdTriggersImprovementSuffix) {
    touch("ACECODE.md");
    std::string s = acecode::build_init_prompt(temp_dir);
    EXPECT_NE(s.find("ACECODE.md already exists"), std::string::npos);
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
    EXPECT_EQ(s.find("ACECODE.md already exists"), std::string::npos);
    // 不应引 LLM 去读不存在的 AGENT.md
    EXPECT_EQ(s.find("AGENT.md"), std::string::npos);
}

// 场景:只有 AGENT.md 时,suffix 点名 AGENT.md,且不提到 CLAUDE.md
TEST_F(InitPromptTest, AgentMdPresentTriggersMigrationSuffix) {
    touch("AGENT.md");
    std::string s = acecode::build_init_prompt(temp_dir);
    EXPECT_NE(s.find("AGENT.md already exists"), std::string::npos);
    EXPECT_NE(s.find("file_write_tool"), std::string::npos);
    EXPECT_EQ(s.find("ACECODE.md already exists"), std::string::npos);
    EXPECT_EQ(s.find("CLAUDE.md"), std::string::npos);
}

// 场景:CLAUDE.md / AGENT.md 同时存在时,prefer AGENT.md 为主基础,CLAUDE.md
// 作为补充。这是为了和 project_instructions 默认 filenames 优先级
// ["ACECODE.md","AGENT.md","CLAUDE.md"] 保持一致,不反转 loader 的读取顺序
TEST_F(InitPromptTest, BothLegacyFilesNamedAgentMdPreferred) {
    touch("CLAUDE.md");
    touch("AGENT.md");
    std::string s = acecode::build_init_prompt(temp_dir);
    // 两个文件都必须出现
    EXPECT_NE(s.find("CLAUDE.md"), std::string::npos);
    EXPECT_NE(s.find("AGENT.md"), std::string::npos);
    // AGENT.md 必须被明确指为主基础(用 "Prefer AGENT.md" 作为稳定锚点)
    EXPECT_NE(s.find("Prefer AGENT.md"), std::string::npos);
    // CLAUDE.md 必须是被交叉检查的补充来源
    EXPECT_NE(s.find("Cross-check CLAUDE.md"), std::string::npos);
}

// 场景:同一个目录状态调用两次,结果必须字节一致。这保证了无隐藏随机性
// (例如没有把 time_t 拼进 prompt),测试可复现
TEST_F(InitPromptTest, DeterminismForFixedState) {
    touch("CLAUDE.md");
    std::string a = acecode::build_init_prompt(temp_dir);
    std::string b = acecode::build_init_prompt(temp_dir);
    EXPECT_EQ(a, b);
}
