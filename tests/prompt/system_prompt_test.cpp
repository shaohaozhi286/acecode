// 覆盖 src/prompt/system_prompt.{hpp,cpp} 的注入逻辑:
// - 没有 memory / project_instructions 输入时,prompt 里不出现对应段头
// - memory 非空时 "# User Memory" 段出现,含 MEMORY.md 原文
// - 有 ACECODE.md 的 cwd 时 "# Project Instructions" 段出现且列出 Source
// - cfg.enabled=false 时即便有文件也不注入

#include <gtest/gtest.h>

#include "config/config.hpp"
#include "memory/memory_paths.hpp"
#include "memory/memory_registry.hpp"
#include "memory/memory_types.hpp"
#include "prompt/system_prompt.hpp"
#include "tool/tool_executor.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
constexpr const char* kHomeEnvName = "USERPROFILE";
#else
constexpr const char* kHomeEnvName = "HOME";
#endif

void set_env(const char* n, const std::string& v) {
#ifdef _WIN32
    _putenv_s(n, v.c_str());
#else
    setenv(n, v.c_str(), 1);
#endif
}

class SystemPromptTest : public ::testing::Test {
protected:
    fs::path temp_home;
    std::string prev_home;

    void SetUp() override {
        const char* e = std::getenv(kHomeEnvName);
        prev_home = e ? e : "";
        temp_home = fs::temp_directory_path() /
                    fs::path("acecode-sysprompt-" +
                             std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::error_code ec;
        fs::remove_all(temp_home, ec);
        fs::create_directories(temp_home);
        set_env(kHomeEnvName, temp_home.string());
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(temp_home, ec);
        set_env(kHomeEnvName, prev_home);
    }
};

void write_file(const fs::path& p, const std::string& c) {
    fs::create_directories(p.parent_path());
    std::ofstream ofs(p, std::ios::binary);
    ofs << c;
}

} // namespace

// 场景:memory / project_instructions 都不提供,prompt 里无对应段头
TEST_F(SystemPromptTest, EmptyInputsOmitSections) {
    acecode::ToolExecutor tools;
    std::string out = acecode::build_system_prompt(tools, temp_home.string());
    EXPECT_EQ(out.find("# User Memory"), std::string::npos);
    EXPECT_EQ(out.find("# Project Instructions"), std::string::npos);
}

// 场景:静态 system prompt 不能包含每次请求都可能变化的上下文,
// 否则 DeepSeek 等 provider 的 prompt cache 前缀会被打穿。
TEST_F(SystemPromptTest, StaticEnvironmentOmitsPerRequestContext) {
    acecode::ToolExecutor tools;
    std::string out = acecode::build_system_prompt(tools, temp_home.string());

    EXPECT_NE(out.find("# Environment"), std::string::npos);
    EXPECT_NE(out.find("- OS: "), std::string::npos);
    EXPECT_NE(out.find("- Shell: "), std::string::npos);
    EXPECT_EQ(out.find("- CWD: "), std::string::npos);
    EXPECT_EQ(out.find("Current local date/time"), std::string::npos);
}

// 场景:动态请求上下文单独构建,由 AgentLoop 放到消息尾部,
// 让模型仍能回答"现在几点"/"当前目录"等问题。
TEST_F(SystemPromptTest, RequestContextIncludesCwdAndCurrentLocalDatetime) {
    std::string out = acecode::build_request_context_prompt(temp_home.string());

    EXPECT_NE(out.find("[当前环境状态]"), std::string::npos);
    EXPECT_NE(out.find("时间："), std::string::npos);
    EXPECT_NE(out.find(" UTC"), std::string::npos);
    EXPECT_NE(out.find("工作目录：" + temp_home.string()), std::string::npos);
}

// 场景:memory 有条目 -> MEMORY.md 非空 -> 注入 User Memory 段
TEST_F(SystemPromptTest, MemorySectionAppearsWhenIndexNonEmpty) {
    fs::create_directories(acecode::get_memory_dir());
    acecode::MemoryRegistry reg;
    reg.scan();
    std::string err;
    reg.upsert("user_profile", acecode::MemoryType::User,
               "senior Go dev", "10y Go\n",
               acecode::MemoryWriteMode::Create, err);

    acecode::ToolExecutor tools;
    acecode::MemoryConfig mcfg;
    std::string out = acecode::build_system_prompt(
        tools, temp_home.string(),
        /*skills=*/nullptr, &reg, &mcfg, /*project=*/nullptr);
    EXPECT_NE(out.find("# User Memory"), std::string::npos);
    EXPECT_NE(out.find("user_profile.md"), std::string::npos);
}

// 场景:memory_cfg.enabled=false 时即使有条目也不注入
TEST_F(SystemPromptTest, MemoryDisabledByCfg) {
    fs::create_directories(acecode::get_memory_dir());
    acecode::MemoryRegistry reg;
    std::string err;
    reg.upsert("x", acecode::MemoryType::User, "x", "x",
               acecode::MemoryWriteMode::Create, err);

    acecode::ToolExecutor tools;
    acecode::MemoryConfig mcfg;
    mcfg.enabled = false;
    std::string out = acecode::build_system_prompt(
        tools, temp_home.string(),
        /*skills=*/nullptr, &reg, &mcfg, /*project=*/nullptr);
    EXPECT_EQ(out.find("# User Memory"), std::string::npos);
}

// 场景:cwd 下有 ACECODE.md -> Project Instructions 段 + Source 行
TEST_F(SystemPromptTest, ProjectInstructionsSectionWithSource) {
    fs::path repo = temp_home / "repo";
    write_file(repo / "ACECODE.md", "# rules\nuse goroutines\n");

    acecode::ToolExecutor tools;
    acecode::ProjectInstructionsConfig pcfg;
    std::string out = acecode::build_system_prompt(
        tools, repo.string(), /*skills=*/nullptr,
        /*memory=*/nullptr, /*memcfg=*/nullptr, &pcfg);
    EXPECT_NE(out.find("# Project Instructions"), std::string::npos);
    EXPECT_NE(out.find("ACECODE.md"), std::string::npos);
    EXPECT_NE(out.find("goroutines"), std::string::npos);
}

// 场景:有 CLAUDE.md 也会被当作 project instructions 注入(compat 路径)
TEST_F(SystemPromptTest, ClaudeMdFallback) {
    fs::path repo = temp_home / "repo";
    write_file(repo / "CLAUDE.md", "# legacy claude rules\n");
    acecode::ToolExecutor tools;
    acecode::ProjectInstructionsConfig pcfg;
    std::string out = acecode::build_system_prompt(
        tools, repo.string(), /*skills=*/nullptr,
        /*memory=*/nullptr, /*memcfg=*/nullptr, &pcfg);
    EXPECT_NE(out.find("# Project Instructions"), std::string::npos);
    EXPECT_NE(out.find("legacy claude rules"), std::string::npos);
    EXPECT_NE(out.find("CLAUDE.md"), std::string::npos);
}

// 场景:cfg.enabled=false 时即便有 ACECODE.md 也不注入
TEST_F(SystemPromptTest, ProjectInstructionsDisabledByCfg) {
    fs::path repo = temp_home / "repo";
    write_file(repo / "ACECODE.md", "content\n");
    acecode::ToolExecutor tools;
    acecode::ProjectInstructionsConfig pcfg;
    pcfg.enabled = false;
    std::string out = acecode::build_system_prompt(
        tools, repo.string(), /*skills=*/nullptr,
        /*memory=*/nullptr, /*memcfg=*/nullptr, &pcfg);
    EXPECT_EQ(out.find("# Project Instructions"), std::string::npos);
}

// 场景:prompt 必须包含 "# Task completion protocol" 段 + 工具名,
// 并且明确 AskUserQuestion 不是"交还控制权"的工具。
TEST_F(SystemPromptTest, TaskCompletionProtocolAppears) {
    acecode::ToolExecutor tools;
    std::string out = acecode::build_system_prompt(tools, temp_home.string());
    EXPECT_NE(out.find("# Task completion protocol"), std::string::npos);
    EXPECT_NE(out.find("task_complete"), std::string::npos);
    EXPECT_NE(out.find("AskUserQuestion"), std::string::npos);
    // 反对 "should I proceed?" 类 prose 问题
    EXPECT_NE(out.find("Should I proceed"), std::string::npos);
    // 必须说清 AskUserQuestion 不是终止器,是辅助决策工具
    EXPECT_NE(out.find("NOT a way to hand control back"),
              std::string::npos);
}

// 场景:Windows 平台 build prompt 必须注入 "# Shell Command Guidance (Windows)" 段。
// 回归测试:用户在 acecode 里让 LLM 跑 `mkdir -p testfolder1`,因为 bash_tool
// 在 Windows 上走 cmd.exe /c,cmd.exe 的 mkdir 不认 -p,把 -p 当成第二个目录名,
// 结果创建出 "-p/" 和 "testfolder1/" 两个目录(claudecodehaha 没这问题是因为它
// 强制走 git-bash)。方案 C:不换 shell,改提示词把高频 cmd.exe vs POSIX 分歧
// 写进 system prompt。这里特意 assert "mkdir -p" 反例本身,防止有人把 guidance
// 段瘦身时把这条最关键的反例删掉。
TEST_F(SystemPromptTest, WindowsShellGuidanceInjected) {
#ifdef _WIN32
    acecode::ToolExecutor tools;
    std::string out = acecode::build_system_prompt(tools, temp_home.string());
    EXPECT_NE(out.find("# Shell Command Guidance (Windows)"), std::string::npos);
    EXPECT_NE(out.find("mkdir -p"), std::string::npos);
    EXPECT_NE(out.find("rd /s /q"), std::string::npos);
    EXPECT_NE(out.find("%VAR%"), std::string::npos);
#else
    GTEST_SKIP() << "Windows-only guidance";
#endif
}

// 场景:POSIX 平台(Linux/macOS)build prompt 时,Windows-only 段必须不出现,
// 避免污染普通用户的 prompt 浪费 token + 误导 LLM。
TEST_F(SystemPromptTest, PosixPromptStaysCleanOfWindowsGuidance) {
#ifndef _WIN32
    acecode::ToolExecutor tools;
    std::string out = acecode::build_system_prompt(tools, temp_home.string());
    EXPECT_EQ(out.find("# Shell Command Guidance (Windows)"), std::string::npos);
    EXPECT_EQ(out.find("cmd.exe"), std::string::npos);
#else
    GTEST_SKIP() << "POSIX-only assertion";
#endif
}

// 场景:acecode 以软件工程为主能力,但不应把"非代码"当作拒绝理由
TEST_F(SystemPromptTest, GeneralNonCodeRequestsAreAllowed) {
    acecode::ToolExecutor tools;
    std::string out = acecode::build_system_prompt(tools, temp_home.string());

    EXPECT_NE(out.find("primary product capability"), std::string::npos);
    EXPECT_NE(out.find("not a restriction"), std::string::npos);
    EXPECT_NE(out.find("not about code"), std::string::npos);
    EXPECT_NE(out.find("not tied to the current project"), std::string::npos);
    EXPECT_NE(out.find("non-code help"), std::string::npos);
    EXPECT_NE(out.find("forcing them into a codebase frame"), std::string::npos);
}
