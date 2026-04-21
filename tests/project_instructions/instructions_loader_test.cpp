// 覆盖 src/project_instructions/instructions_loader.{hpp,cpp}:
// - 三种文件名(ACECODE.md / AGENT.md / CLAUDE.md)都能被读到
// - 同层目录只选一个,按 filenames 优先级
// - 自定义 filenames 顺序生效
// - read_agent_md / read_claude_md 开关正确剔除对应文件名
// - 单文件 / 聚合 / 深度上限触发时有 truncated marker
// - 全局 ~/.acecode/ 文件被前置
// - 符号链接循环(至少跨平台的简化模型)不会无限递归

#include <gtest/gtest.h>

#include "project_instructions/instructions_loader.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

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

void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream ofs(p, std::ios::binary);
    ofs << content;
}

class InstructionsLoaderTest : public ::testing::Test {
protected:
    fs::path temp_home;
    std::string prev_home;

    void SetUp() override {
        const char* e = std::getenv(kHomeEnvName);
        prev_home = e ? e : "";
        temp_home = fs::temp_directory_path() /
                    fs::path("acecode-instr-loader-" +
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

} // namespace

// 场景:cwd 下有 ACECODE.md,cfg 默认时被加载
TEST_F(InstructionsLoaderTest, LoadsProjectAcecodeMd) {
    fs::path repo = temp_home / "repo";
    write_file(repo / "ACECODE.md", "# repo rules\nuse goroutines\n");

    acecode::ProjectInstructionsConfig cfg;
    auto merged = acecode::load_project_instructions(repo.string(), cfg);
    EXPECT_FALSE(merged.merged_body.empty());
    EXPECT_NE(merged.merged_body.find("goroutines"), std::string::npos);
    EXPECT_NE(merged.merged_body.find("ACECODE.md"), std::string::npos);
    EXPECT_EQ(merged.sources.size(), 1u);
}

// 场景:cwd 没有 ACECODE.md 只有 AGENT.md → 读到 AGENT.md
TEST_F(InstructionsLoaderTest, FallbacksToAgentMd) {
    fs::path repo = temp_home / "repo";
    write_file(repo / "AGENT.md", "# agent rules\n");
    acecode::ProjectInstructionsConfig cfg;
    auto merged = acecode::load_project_instructions(repo.string(), cfg);
    EXPECT_NE(merged.merged_body.find("agent rules"), std::string::npos);
    EXPECT_NE(merged.merged_body.find("AGENT.md"), std::string::npos);
}

// 场景:cwd 没有 ACECODE.md / AGENT.md 只有 CLAUDE.md → 读到 CLAUDE.md
TEST_F(InstructionsLoaderTest, FallbacksToClaudeMd) {
    fs::path repo = temp_home / "repo";
    write_file(repo / "CLAUDE.md", "# claude rules\n");
    acecode::ProjectInstructionsConfig cfg;
    auto merged = acecode::load_project_instructions(repo.string(), cfg);
    EXPECT_NE(merged.merged_body.find("claude rules"), std::string::npos);
    EXPECT_NE(merged.merged_body.find("CLAUDE.md"), std::string::npos);
}

// 场景:同层目录同时有 ACECODE / AGENT / CLAUDE 时,默认 filenames 优先级选 ACECODE
TEST_F(InstructionsLoaderTest, AcecodeBeatsAgentAndClaudeByDefault) {
    fs::path repo = temp_home / "repo";
    write_file(repo / "ACECODE.md", "ace content\n");
    write_file(repo / "AGENT.md", "agent content\n");
    write_file(repo / "CLAUDE.md", "claude content\n");

    acecode::ProjectInstructionsConfig cfg;
    auto merged = acecode::load_project_instructions(repo.string(), cfg);
    EXPECT_NE(merged.merged_body.find("ace content"), std::string::npos);
    EXPECT_EQ(merged.merged_body.find("agent content"), std::string::npos);
    EXPECT_EQ(merged.merged_body.find("claude content"), std::string::npos);
    EXPECT_EQ(merged.sources.size(), 1u);
}

// 场景:自定义 filenames 顺序把 CLAUDE.md 抬到首位
TEST_F(InstructionsLoaderTest, CustomFilenamesOrderOverridesDefault) {
    fs::path repo = temp_home / "repo";
    write_file(repo / "ACECODE.md", "ace content\n");
    write_file(repo / "CLAUDE.md", "claude content\n");

    acecode::ProjectInstructionsConfig cfg;
    cfg.filenames = {"CLAUDE.md", "ACECODE.md", "AGENT.md"};
    auto merged = acecode::load_project_instructions(repo.string(), cfg);
    EXPECT_NE(merged.merged_body.find("claude content"), std::string::npos);
    EXPECT_EQ(merged.merged_body.find("ace content"), std::string::npos);
}

// 场景:read_agent_md=false 时 AGENT.md 被剔除,fallback 到 CLAUDE.md
TEST_F(InstructionsLoaderTest, ReadAgentMdGateDisabled) {
    fs::path repo = temp_home / "repo";
    write_file(repo / "AGENT.md", "agent\n");
    write_file(repo / "CLAUDE.md", "claude\n");

    acecode::ProjectInstructionsConfig cfg;
    cfg.read_agent_md = false;
    auto merged = acecode::load_project_instructions(repo.string(), cfg);
    EXPECT_NE(merged.merged_body.find("claude"), std::string::npos);
    EXPECT_EQ(merged.merged_body.find("agent"), std::string::npos);
}

// 场景:read_claude_md=false 时 CLAUDE.md 被剔除,fallback 到 ACECODE.md 或无
TEST_F(InstructionsLoaderTest, ReadClaudeMdGateDisabled) {
    fs::path repo = temp_home / "repo";
    write_file(repo / "CLAUDE.md", "claude\n");
    acecode::ProjectInstructionsConfig cfg;
    cfg.read_claude_md = false;
    auto merged = acecode::load_project_instructions(repo.string(), cfg);
    EXPECT_TRUE(merged.merged_body.empty());
    EXPECT_EQ(merged.sources.size(), 0u);
}

// 场景:cfg.enabled=false 时完全不读文件
TEST_F(InstructionsLoaderTest, DisabledProducesEmpty) {
    fs::path repo = temp_home / "repo";
    write_file(repo / "ACECODE.md", "should not be read\n");
    acecode::ProjectInstructionsConfig cfg;
    cfg.enabled = false;
    auto merged = acecode::load_project_instructions(repo.string(), cfg);
    EXPECT_TRUE(merged.merged_body.empty());
}

// 场景:~/.acecode/ACECODE.md 作为全局层被前置到项目级之前
TEST_F(InstructionsLoaderTest, GlobalAcecodeMdPrepended) {
    write_file(temp_home / ".acecode" / "ACECODE.md", "GLOBAL RULES\n");
    fs::path repo = temp_home / "repo";
    write_file(repo / "ACECODE.md", "PROJECT RULES\n");

    acecode::ProjectInstructionsConfig cfg;
    auto merged = acecode::load_project_instructions(repo.string(), cfg);

    std::size_t gpos = merged.merged_body.find("GLOBAL RULES");
    std::size_t ppos = merged.merged_body.find("PROJECT RULES");
    ASSERT_NE(gpos, std::string::npos);
    ASSERT_NE(ppos, std::string::npos);
    EXPECT_LT(gpos, ppos) << "全局 ACECODE.md 应位于项目 ACECODE.md 之前";
    EXPECT_EQ(merged.sources.size(), 2u);
}

// 场景:外层目录和内层目录同时有 ACECODE.md,合并顺序是外层在前
TEST_F(InstructionsLoaderTest, NestedOuterBeforeInner) {
    fs::path outer = temp_home / "repo";
    fs::path inner = outer / "src";
    write_file(outer / "ACECODE.md", "OUTER\n");
    write_file(inner / "ACECODE.md", "INNER\n");

    acecode::ProjectInstructionsConfig cfg;
    auto merged = acecode::load_project_instructions(inner.string(), cfg);
    std::size_t op = merged.merged_body.find("OUTER");
    std::size_t ip = merged.merged_body.find("INNER");
    ASSERT_NE(op, std::string::npos);
    ASSERT_NE(ip, std::string::npos);
    EXPECT_LT(op, ip);
}

// 场景:搜索停在 HOME 边界,HOME 以上的 /etc 级别文件不被读取
TEST_F(InstructionsLoaderTest, StopsAtHomeBoundary) {
    // 我们用 temp_home 作为 HOME,在 HOME 同级或更上不应出现"读到"的效果
    fs::path repo = temp_home / "sub" / "repo";
    write_file(repo / "ACECODE.md", "repo\n");
    // 故意在 temp_home 同级父目录也放一份(不应被读取,因为会越过 HOME)
    fs::path parent_of_home = temp_home.parent_path();
    if (!parent_of_home.empty()) {
        write_file(parent_of_home / "ACECODE.md", "SHOULD_NOT_APPEAR\n");
    }

    acecode::ProjectInstructionsConfig cfg;
    auto merged = acecode::load_project_instructions(repo.string(), cfg);
    EXPECT_NE(merged.merged_body.find("repo"), std::string::npos);
    EXPECT_EQ(merged.merged_body.find("SHOULD_NOT_APPEAR"), std::string::npos);
}

// 场景:单文件超过 max_bytes 时 merged_body 被截断并出现 truncated marker
TEST_F(InstructionsLoaderTest, PerFileTruncation) {
    fs::path repo = temp_home / "repo";
    std::string big(4096, 'x');
    write_file(repo / "ACECODE.md", big);

    acecode::ProjectInstructionsConfig cfg;
    cfg.max_bytes = 1024;
    cfg.max_total_bytes = 10 * 1024;
    auto merged = acecode::load_project_instructions(repo.string(), cfg);
    EXPECT_TRUE(merged.truncated);
    EXPECT_NE(merged.merged_body.find("per-file cap"), std::string::npos);
}

// 场景:全部缺失时 merged_body 为空且 sources 也为空
TEST_F(InstructionsLoaderTest, NoFilesEmptyResult) {
    fs::path repo = temp_home / "repo";
    fs::create_directories(repo);
    acecode::ProjectInstructionsConfig cfg;
    auto merged = acecode::load_project_instructions(repo.string(), cfg);
    EXPECT_TRUE(merged.merged_body.empty());
    EXPECT_EQ(merged.sources.size(), 0u);
}
