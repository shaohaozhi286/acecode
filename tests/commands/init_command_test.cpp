// 覆盖 src/commands/init_command.{hpp,cpp} 的纯逻辑 build_acecode_md_skeleton:
// - 目录下没有 CLAUDE.md / AGENT.md 时骨架不含迁移注释
// - 只有 CLAUDE.md 时骨架含 CLAUDE.md 提示
// - 只有 AGENT.md 时骨架含 AGENT.md 提示
// - 两个文件都存在时两个名字都出现在提示块里
//
// 完整 /init 命令的 CommandContext 组装依赖 AgentLoop 等运行时对象,不适合单元测试,
// 只在手动端到端验证阶段覆盖

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
