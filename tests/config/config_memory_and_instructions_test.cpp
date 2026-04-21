// 覆盖 src/config/config.{hpp,cpp} 中本 change 新增的 memory / project_instructions
// 段的序列化与校验语义：
// - 缺省字段时,默认值符合 design D8
// - 合法/非法字段能被 validate_config 正确分流
// - filenames 为空数组时不抹掉结构体默认,保证向后兼容

#include <gtest/gtest.h>

#include "config/config.hpp"

#include <nlohmann/json.hpp>

using namespace acecode;

namespace {

// 模拟 config.cpp 中 load_config() 对 memory 段的解析逻辑,避免依赖真实文件系统
void apply_memory_section(const nlohmann::json& mj, MemoryConfig& out) {
    if (mj.contains("enabled") && mj["enabled"].is_boolean())
        out.enabled = mj["enabled"].get<bool>();
    if (mj.contains("max_index_bytes") && mj["max_index_bytes"].is_number_integer()) {
        long long v = mj["max_index_bytes"].get<long long>();
        if (v > 0) out.max_index_bytes = static_cast<std::size_t>(v);
    }
}

// 同上,对 project_instructions 段
void apply_project_instructions_section(const nlohmann::json& pj,
                                        ProjectInstructionsConfig& out) {
    if (pj.contains("enabled") && pj["enabled"].is_boolean())
        out.enabled = pj["enabled"].get<bool>();
    if (pj.contains("max_depth") && pj["max_depth"].is_number_integer()) {
        int v = pj["max_depth"].get<int>();
        if (v > 0) out.max_depth = v;
    }
    if (pj.contains("max_bytes") && pj["max_bytes"].is_number_integer()) {
        long long v = pj["max_bytes"].get<long long>();
        if (v > 0) out.max_bytes = static_cast<std::size_t>(v);
    }
    if (pj.contains("max_total_bytes") && pj["max_total_bytes"].is_number_integer()) {
        long long v = pj["max_total_bytes"].get<long long>();
        if (v > 0) out.max_total_bytes = static_cast<std::size_t>(v);
    }
    if (pj.contains("filenames") && pj["filenames"].is_array()) {
        std::vector<std::string> fns;
        for (const auto& v : pj["filenames"]) {
            if (v.is_string()) {
                std::string s = v.get<std::string>();
                if (!s.empty()) fns.push_back(std::move(s));
            }
        }
        if (!fns.empty()) out.filenames = std::move(fns);
    }
    if (pj.contains("read_agent_md") && pj["read_agent_md"].is_boolean())
        out.read_agent_md = pj["read_agent_md"].get<bool>();
    if (pj.contains("read_claude_md") && pj["read_claude_md"].is_boolean())
        out.read_claude_md = pj["read_claude_md"].get<bool>();
}

} // namespace

// 场景:memory / project_instructions 段缺失时,结构体字段全部保持默认值
TEST(ConfigMemoryDefaults, StructDefaults) {
    MemoryConfig mem;
    EXPECT_TRUE(mem.enabled);
    EXPECT_EQ(mem.max_index_bytes, 32u * 1024);

    ProjectInstructionsConfig pi;
    EXPECT_TRUE(pi.enabled);
    EXPECT_EQ(pi.max_depth, 8);
    EXPECT_EQ(pi.max_bytes, 256u * 1024);
    EXPECT_EQ(pi.max_total_bytes, 1024u * 1024);
    // 默认 filenames 顺序明确 ACECODE > AGENT > CLAUDE
    ASSERT_EQ(pi.filenames.size(), 3u);
    EXPECT_EQ(pi.filenames[0], "ACECODE.md");
    EXPECT_EQ(pi.filenames[1], "AGENT.md");
    EXPECT_EQ(pi.filenames[2], "CLAUDE.md");
    EXPECT_TRUE(pi.read_agent_md);
    EXPECT_TRUE(pi.read_claude_md);
}

// 场景:显式写入 memory 段的合法值能被正确解析
TEST(ConfigMemoryParse, ExplicitFieldsAccepted) {
    auto j = nlohmann::json::parse(R"({"enabled":false,"max_index_bytes":65536})");
    MemoryConfig mem;
    apply_memory_section(j, mem);
    EXPECT_FALSE(mem.enabled);
    EXPECT_EQ(mem.max_index_bytes, 65536u);
}

// 场景:project_instructions 段可以关掉 AGENT.md / CLAUDE.md 读取开关
TEST(ConfigProjectInstructionsParse, ToggleSwitches) {
    auto j = nlohmann::json::parse(R"({"read_agent_md":false,"read_claude_md":false})");
    ProjectInstructionsConfig pi;
    apply_project_instructions_section(j, pi);
    EXPECT_FALSE(pi.read_agent_md);
    EXPECT_FALSE(pi.read_claude_md);
    // filenames 数组没有显式传入,应保持默认三项
    EXPECT_EQ(pi.filenames.size(), 3u);
}

// 场景:自定义 filenames 顺序生效(例如团队想让 CLAUDE.md 优先)
TEST(ConfigProjectInstructionsParse, CustomFilenamesOrder) {
    auto j = nlohmann::json::parse(R"({"filenames":["CLAUDE.md","ACECODE.md"]})");
    ProjectInstructionsConfig pi;
    apply_project_instructions_section(j, pi);
    ASSERT_EQ(pi.filenames.size(), 2u);
    EXPECT_EQ(pi.filenames[0], "CLAUDE.md");
    EXPECT_EQ(pi.filenames[1], "ACECODE.md");
}

// 场景:显式给了空数组时回退到默认三项(避免 misconfig 把项目指令完全关闭)
TEST(ConfigProjectInstructionsParse, EmptyFilenamesFallsBackToDefault) {
    auto j = nlohmann::json::parse(R"({"filenames":[]})");
    ProjectInstructionsConfig pi;
    apply_project_instructions_section(j, pi);
    ASSERT_EQ(pi.filenames.size(), 3u);
    EXPECT_EQ(pi.filenames[0], "ACECODE.md");
}

// 场景:filenames 里混入空字符串时,空项被忽略,非空项正常进入
TEST(ConfigProjectInstructionsParse, FilenamesSkipsEmptyEntries) {
    auto j = nlohmann::json::parse(R"({"filenames":["","AGENT.md",""]})");
    ProjectInstructionsConfig pi;
    apply_project_instructions_section(j, pi);
    ASSERT_EQ(pi.filenames.size(), 1u);
    EXPECT_EQ(pi.filenames[0], "AGENT.md");
}

// 场景:validate_config 对 memory / project_instructions 非法字段的捕获
TEST(ConfigValidation, MemoryMaxIndexBytesMustBePositive) {
    AppConfig cfg;
    cfg.memory.max_index_bytes = 0;
    auto errs = validate_config(cfg);
    bool found = false;
    for (const auto& e : errs) {
        if (e.find("memory.max_index_bytes") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(ConfigValidation, ProjectInstructionsMaxDepthMustBeAtLeastOne) {
    AppConfig cfg;
    cfg.project_instructions.max_depth = 0;
    auto errs = validate_config(cfg);
    bool found = false;
    for (const auto& e : errs) {
        if (e.find("max_depth") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(ConfigValidation, ProjectInstructionsTotalMustNotBeLessThanPerFile) {
    AppConfig cfg;
    cfg.project_instructions.max_bytes = 1024 * 1024;
    cfg.project_instructions.max_total_bytes = 512 * 1024; // 小于 max_bytes
    auto errs = validate_config(cfg);
    bool found = false;
    for (const auto& e : errs) {
        if (e.find("max_total_bytes") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(ConfigValidation, ProjectInstructionsFilenamesMustNotContainSeparators) {
    AppConfig cfg;
    cfg.project_instructions.filenames = {"ACECODE.md", "sub/AGENT.md"};
    auto errs = validate_config(cfg);
    bool found = false;
    for (const auto& e : errs) {
        if (e.find("path separator") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(ConfigValidation, ProjectInstructionsFilenamesMustNotBeEmpty) {
    AppConfig cfg;
    cfg.project_instructions.filenames = {""};
    auto errs = validate_config(cfg);
    bool found = false;
    for (const auto& e : errs) {
        if (e.find("empty entry") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

// 场景:validate_config 对完全默认的 AppConfig 不报任何 memory/project_instructions 错
TEST(ConfigValidation, DefaultConfigPassesNewChecks) {
    AppConfig cfg;
    auto errs = validate_config(cfg);
    for (const auto& e : errs) {
        EXPECT_EQ(e.find("memory."), std::string::npos) << e;
        EXPECT_EQ(e.find("project_instructions."), std::string::npos) << e;
    }
}
