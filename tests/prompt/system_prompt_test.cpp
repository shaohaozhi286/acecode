// 覆盖 src/prompt/system_prompt.{hpp,cpp} 的 prompt cache 分区逻辑:
// - 静态 system prompt 不包含每次请求/会话可能变化的 context
// - memory / project_instructions 进入 provider-facing session context
// - full tool schema 只走 provider tools array,不重复塞进 system prompt

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
#include <vector>

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

// 场景:memory 有条目 -> MEMORY.md 非空 -> 进入 session context,
// 静态 system prompt 保持不含 User Memory。
TEST_F(SystemPromptTest, MemoryContextAppearsWhenIndexNonEmpty) {
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
    EXPECT_EQ(out.find("# User Memory"), std::string::npos);
    EXPECT_EQ(out.find("user_profile.md"), std::string::npos);

    auto context = acecode::build_user_memory_context_prompt(&reg, &mcfg);
    EXPECT_NE(context.content.find("# User Memory"), std::string::npos);
    EXPECT_NE(context.content.find("user_profile.md"), std::string::npos);
    EXPECT_FALSE(context.cache_key.empty());
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

    auto context = acecode::build_user_memory_context_prompt(&reg, &mcfg);
    EXPECT_TRUE(context.content.empty());
}

// 场景:cwd 下有 AGENT.md -> provider-facing Project Instructions context,
// 静态 system prompt 不包含项目文件内容。
TEST_F(SystemPromptTest, ProjectInstructionsContextWithSource) {
    fs::path repo = temp_home / "repo";
    write_file(repo / "AGENT.md", "# rules\nuse goroutines\n");

    acecode::ToolExecutor tools;
    acecode::ProjectInstructionsConfig pcfg;
    std::string out = acecode::build_system_prompt(
        tools, repo.string(), /*skills=*/nullptr,
        /*memory=*/nullptr, /*memcfg=*/nullptr, &pcfg);
    EXPECT_EQ(out.find("# Project Instructions"), std::string::npos);
    EXPECT_EQ(out.find("goroutines"), std::string::npos);

    auto context = acecode::build_project_instructions_context_prompt(repo.string(), &pcfg);
    EXPECT_NE(context.content.find("# Project Instructions"), std::string::npos);
    EXPECT_NE(context.content.find("AGENT.md"), std::string::npos);
    EXPECT_NE(context.content.find("goroutines"), std::string::npos);
    EXPECT_FALSE(context.cache_key.empty());
}

// 场景:有 CLAUDE.md 也会被当作 project instructions context(compat 路径)
TEST_F(SystemPromptTest, ClaudeMdFallback) {
    fs::path repo = temp_home / "repo";
    write_file(repo / "CLAUDE.md", "# legacy claude rules\n");
    acecode::ToolExecutor tools;
    acecode::ProjectInstructionsConfig pcfg;
    std::string out = acecode::build_system_prompt(
        tools, repo.string(), /*skills=*/nullptr,
        /*memory=*/nullptr, /*memcfg=*/nullptr, &pcfg);
    EXPECT_EQ(out.find("# Project Instructions"), std::string::npos);
    EXPECT_EQ(out.find("legacy claude rules"), std::string::npos);

    auto context = acecode::build_project_instructions_context_prompt(repo.string(), &pcfg);
    EXPECT_NE(context.content.find("# Project Instructions"), std::string::npos);
    EXPECT_NE(context.content.find("legacy claude rules"), std::string::npos);
    EXPECT_NE(context.content.find("CLAUDE.md"), std::string::npos);
}

// 场景:cfg.enabled=false 时即便有 AGENT.md 也不注入
TEST_F(SystemPromptTest, ProjectInstructionsDisabledByCfg) {
    fs::path repo = temp_home / "repo";
    write_file(repo / "AGENT.md", "content\n");
    acecode::ToolExecutor tools;
    acecode::ProjectInstructionsConfig pcfg;
    pcfg.enabled = false;
    std::string out = acecode::build_system_prompt(
        tools, repo.string(), /*skills=*/nullptr,
        /*memory=*/nullptr, /*memcfg=*/nullptr, &pcfg);
    EXPECT_EQ(out.find("# Project Instructions"), std::string::npos);

    auto context = acecode::build_project_instructions_context_prompt(repo.string(), &pcfg);
    EXPECT_TRUE(context.content.empty());
}

TEST_F(SystemPromptTest, CustomInstructionsContextAppearsWhenNonEmpty) {
    acecode::ToolExecutor tools;
    acecode::CustomInstructionsConfig custom_cfg;
    custom_cfg.text = "Always answer in concise Chinese.";

    std::string out = acecode::build_system_prompt(tools, temp_home.string());
    EXPECT_EQ(out.find("# Custom Instructions"), std::string::npos);
    EXPECT_EQ(out.find("concise Chinese"), std::string::npos);

    auto context = acecode::build_custom_instructions_context_prompt(&custom_cfg);
    EXPECT_NE(context.content.find("# Custom Instructions"), std::string::npos);
    EXPECT_NE(context.content.find("concise Chinese"), std::string::npos);
    EXPECT_NE(context.content.find("do not override"), std::string::npos);
    EXPECT_FALSE(context.cache_key.empty());
}

TEST_F(SystemPromptTest, CustomInstructionsOmittedWhenWhitespaceOnly) {
    acecode::CustomInstructionsConfig custom_cfg;
    custom_cfg.text = " \n\t ";

    auto context = acecode::build_custom_instructions_context_prompt(&custom_cfg);
    EXPECT_TRUE(context.content.empty());
    EXPECT_TRUE(context.cache_key.empty());
}

TEST_F(SystemPromptTest, SessionContextIncludesCustomInstructions) {
    acecode::CustomInstructionsConfig custom_cfg;
    custom_cfg.text = "Use repository-specific wording.";

    auto context = acecode::build_session_context_prompt(
        temp_home.string(),
        /*memory=*/nullptr,
        /*memory_cfg=*/nullptr,
        /*project_instructions_cfg=*/nullptr,
        /*skills=*/nullptr,
        /*context_window_tokens=*/0,
        &custom_cfg);

    EXPECT_NE(context.content.find("<system-reminder>"), std::string::npos);
    EXPECT_NE(context.content.find("# Custom Instructions"), std::string::npos);
    EXPECT_NE(context.content.find("repository-specific wording"), std::string::npos);
    EXPECT_FALSE(context.cache_key.empty());
}

// 场景:full tool schema 不再重复塞进静态 system prompt,避免工具 schema 变化
// 打穿前缀缓存。结构化 schema 仍由 provider tools array 发送。
TEST_F(SystemPromptTest, StaticPromptDoesNotDuplicateToolSchemas) {
    acecode::ToolExecutor tools;
    acecode::ToolDef def;
    def.name = "example_tool";
    def.description = "Example tool description.";
    def.parameters = {
        {"type", "object"},
        {"properties", {
            {"path", {{"type", "string"}, {"description", "Path to read"}}}
        }},
    };
    acecode::ToolImpl impl;
    impl.definition = def;
    impl.execute = [](const std::string&, const acecode::ToolContext&) {
        return acecode::ToolResult{"ok", true};
    };
    tools.register_tool(impl);

    std::string out = acecode::build_system_prompt(tools, temp_home.string());
    EXPECT_NE(out.find("# Tool Schemas"), std::string::npos);
    EXPECT_EQ(out.find("## example_tool"), std::string::npos);
    EXPECT_EQ(out.find("Parameters:"), std::string::npos);
    EXPECT_EQ(out.find("\"properties\""), std::string::npos);
}

// 场景:hash helper 稳定区分静态 prompt / mutable context / tools 三类变化。
TEST_F(SystemPromptTest, PromptCacheDiagnosticsSeparatePromptContextAndTools) {
    acecode::ToolDef tool_a;
    tool_a.name = "a";
    tool_a.description = "A";
    tool_a.parameters = {{"type", "object"}};
    acecode::ToolDef tool_b = tool_a;
    tool_b.description = "B";

    std::string static_prompt = "stable";
    auto d1 = acecode::build_prompt_cache_diagnostics(static_prompt, "ctx1", {tool_a});
    auto d2 = acecode::build_prompt_cache_diagnostics(static_prompt, "ctx2", {tool_a});
    auto d3 = acecode::build_prompt_cache_diagnostics(static_prompt, "ctx1", {tool_b});

    EXPECT_EQ(d1.static_system_prompt_hash, d2.static_system_prompt_hash);
    EXPECT_NE(d1.mutable_context_hash, d2.mutable_context_hash);
    EXPECT_EQ(d1.tool_schema_hash, d2.tool_schema_hash);

    EXPECT_EQ(d1.static_system_prompt_hash, d3.static_system_prompt_hash);
    EXPECT_EQ(d1.mutable_context_hash, d3.mutable_context_hash);
    EXPECT_NE(d1.tool_schema_hash, d3.tool_schema_hash);
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

// 场景:工具使用与进度更新文案应鼓励同一 assistant turn 中批量发出独立工具调用,
// 而不是形成一句旁白配一个工具调用的低密度交替模式。
TEST_F(SystemPromptTest, PromptEncouragesBatchedToolCallsWithoutPerCallNarration) {
    acecode::ToolExecutor tools;
    std::string out = acecode::build_system_prompt(tools, temp_home.string());

    EXPECT_NE(out.find("batch them in the same assistant message"),
              std::string::npos);
    EXPECT_NE(out.find("Do not add a progress sentence before each individual tool call"),
              std::string::npos);
    EXPECT_NE(out.find("Do not narrate every tool call"),
              std::string::npos);
    EXPECT_NE(out.find("prefer silent batches of tool calls"),
              std::string::npos);
    EXPECT_NE(out.find("\"Let me read this file.\" followed by one file_read"),
              std::string::npos);
    EXPECT_EQ(out.find("you will produce many assistant messages between tool calls"),
              std::string::npos);
}

TEST_F(SystemPromptTest, PromptUsesClaudeStyleReadFailureGuidanceAndGuidesScratchScripts) {
    acecode::ToolExecutor tools;
    std::string out = acecode::build_system_prompt(tools, temp_home.string());

    EXPECT_NE(out.find("file_edit will error if you attempt an edit without reading the file"), std::string::npos);
    EXPECT_NE(out.find("file_write will fail if you did not read the file first"), std::string::npos);
    EXPECT_NE(out.find("Do not call file_read again for the same file/range"), std::string::npos);
    EXPECT_NE(out.find("Do not re-read a file only to verify a successful edit/write"), std::string::npos);
    EXPECT_NE(out.find("ACECODE_TMPDIR"), std::string::npos);
    EXPECT_EQ(out.find("Before editing or overwriting an existing non-empty file, read the target file first"), std::string::npos);
    EXPECT_EQ(out.find("partial reads are only enough for range edits"), std::string::npos);
    EXPECT_EQ(out.find("start_line/end_line/expected_hash"), std::string::npos);
    EXPECT_EQ(out.find("metadata/range edit"), std::string::npos);
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
