// 覆盖 src/prompt/system_prompt.{hpp,cpp} 的 skill 索引注入逻辑
// (openspec/changes/inject-skill-index-into-context):
// - 索引格式化:category 分组 / whenToUse 拼接 / 单条 250 字符 UTF-8 安全截断
// - 预算退化:超预算 → names-only → 截尾 + "(+N more)" marker
// - PromptContextBlock 装配:空 registry 不发块,cache_key 跟随 skill 集变化
// - session context 集成:skills 块进 system-reminder
// - 静态 system prompt 措辞:指向索引而非 skills_list 枚举
//
// 背景 bug:此前 skill 清单从不进入模型上下文,模型对已安装 skill 零可见,
// 永远不会主动调用 skills_list / skill_view —— skill 只能靠用户显式 /<name>
// 触发。修复后索引每请求注入 session-context system-reminder。

#include <gtest/gtest.h>

#include "prompt/system_prompt.hpp"
#include "skills/skill_metadata.hpp"
#include "skills/skill_registry.hpp"
#include "tool/tool_executor.hpp"
#include "utils/utf8_path.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

acecode::SkillMetadata make_skill(const std::string& name,
                                  const std::string& description,
                                  const std::string& category = "",
                                  const std::string& when_to_use = "") {
    acecode::SkillMetadata meta;
    meta.name = name;
    meta.command_key = name;
    meta.description = description;
    meta.category = category;
    meta.when_to_use = when_to_use;
    return meta;
}

void write_skill_md(const fs::path& root,
                    const std::string& category,
                    const std::string& name,
                    const std::string& description,
                    const std::string& when_to_use = "") {
    fs::path dir = root;
    if (!category.empty()) dir /= acecode::path_from_utf8(category);
    dir /= acecode::path_from_utf8(name);
    fs::create_directories(dir);
    std::ofstream ofs(dir / "SKILL.md", std::ios::binary);
    ofs << "---\n"
        << "name: " << name << "\n"
        << "description: " << description << "\n";
    if (!when_to_use.empty()) ofs << "whenToUse: " << when_to_use << "\n";
    ofs << "---\n\n# " << name << "\n";
}

class SkillsIndexRegistryTest : public ::testing::Test {
protected:
    fs::path temp_root;

    void SetUp() override {
        temp_root = fs::temp_directory_path() /
                    fs::path("acecode-skills-index-test-" +
                             std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::error_code ec;
        fs::remove_all(temp_root, ec);
        fs::create_directories(temp_root);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(temp_root, ec);
    }
};

// ---------------------------------------------------------------------------
// skills_index_char_budget
// ---------------------------------------------------------------------------

// 触发场景:已知 context window(128k tokens)。
// 期望:预算 = 1% × 4 chars/token = 128000 * 4 / 100 = 5120 字符。
TEST(SkillsIndexBudgetTest, KnownWindowGivesOnePercentInChars) {
    EXPECT_EQ(acecode::skills_index_char_budget(128000), 5120u);
    EXPECT_EQ(acecode::skills_index_char_budget(200000), 8000u);
}

// 触发场景:context window 未知(0 或负数,如 provider 探测失败)。
// 期望:退回 8000 字符兜底(≈ 200k 窗口的 1%),与 claude-code 默认一致。
TEST(SkillsIndexBudgetTest, UnknownWindowFallsBackTo8000) {
    EXPECT_EQ(acecode::skills_index_char_budget(0), 8000u);
    EXPECT_EQ(acecode::skills_index_char_budget(-1), 8000u);
}

// ---------------------------------------------------------------------------
// format_skills_index_within_budget
// ---------------------------------------------------------------------------

// 触发场景:无 skill。期望:空串(调用方据此跳过整个块)。
TEST(SkillsIndexFormatTest, EmptyListRendersEmpty) {
    EXPECT_EQ(acecode::format_skills_index_within_budget({}, 8000), "");
}

// 触发场景:无 category 与有 category 的 skill 混合。
// 期望:无 category 条目顶格在前;有 category 的按 "<category>:" 标题分组、
// 条目缩进两格 —— 与 hermes <available_skills> 的分组结构一致。
TEST(SkillsIndexFormatTest, GroupsByCategoryWithFlatEntriesFirst) {
    std::vector<acecode::SkillMetadata> skills = {
        make_skill("review-pr", "Review pull requests", "review"),
        make_skill("standalone", "A flat skill"),
    };
    std::string out = acecode::format_skills_index_within_budget(skills, 8000);
    EXPECT_EQ(out,
              "- standalone: A flat skill\n"
              "review:\n"
              "  - review-pr: Review pull requests");
}

// 触发场景:skill 带 whenToUse 触发条件。
// 期望:渲染为 "description — whenToUse"(em-dash 分隔),让模型一眼看到
// "什么时候该用" —— 这是主动触发率的关键信息。
TEST(SkillsIndexFormatTest, AppendsWhenToUseAfterDescription) {
    std::vector<acecode::SkillMetadata> skills = {
        make_skill("commit", "Create a git commit", "", "Use when the user asks to commit changes"),
    };
    std::string out = acecode::format_skills_index_within_budget(skills, 8000);
    EXPECT_EQ(out, "- commit: Create a git commit \xE2\x80\x94 Use when the user asks to commit changes");
}

// 触发场景:描述(含 whenToUse)超过 250 字符单条上限。
// 期望:UTF-8 边界安全截断 + "..." 收尾,整行描述部分 ≤ 250 字节。
// 250 来自 claude-code 的 MAX_LISTING_DESC_CHARS —— 索引只做发现,
// 全文靠 skill_view 加载,冗长描述只浪费 token 不提高匹配率。
TEST(SkillsIndexFormatTest, TruncatesLongDescriptionOnUtf8Boundary) {
    // 用 3 字节中文字符构造 300 字节描述,截断点大概率落在多字节中间。
    std::string long_desc;
    for (int i = 0; i < 100; ++i) long_desc += "\xE4\xB8\xAD"; // "中" × 100
    std::vector<acecode::SkillMetadata> skills = {make_skill("s", long_desc)};
    std::string out = acecode::format_skills_index_within_budget(skills, 8000);

    ASSERT_TRUE(out.rfind("- s: ", 0) == 0);
    std::string desc_part = out.substr(5);
    EXPECT_LE(desc_part.size(), 250u);
    EXPECT_EQ(desc_part.substr(desc_part.size() - 3), "...");
    // 截断后仍是合法 UTF-8:每个 "中" 完整保留,无孤立的延续字节。
    std::string body = desc_part.substr(0, desc_part.size() - 3);
    EXPECT_EQ(body.size() % 3, 0u);
}

// 触发场景:全量渲染超出预算,但 names-only 放得下。
// 期望:整体退化为纯名字清单(保留 category 结构),不再含任何描述。
TEST(SkillsIndexFormatTest, OverBudgetDegradesToNamesOnly) {
    std::vector<acecode::SkillMetadata> skills;
    for (int i = 0; i < 10; ++i) {
        skills.push_back(make_skill("skill-" + std::to_string(i),
                                    std::string(100, 'd'), "cat"));
    }
    // 预算 200:全量(10 × ~110 字符)放不下,names-only(10 × ~14)放得下。
    std::string out = acecode::format_skills_index_within_budget(skills, 200);
    EXPECT_NE(out.find("- skill-0"), std::string::npos);
    EXPECT_EQ(out.find("ddd"), std::string::npos);
    EXPECT_LE(out.size(), 200u);
}

// 触发场景:names-only 仍超预算(极端:海量 skill + 极小窗口)。
// 期望:截尾保留放得下的条目,末尾追加 "(+N more skills — call skills_list
// to see all)" marker,让模型知道清单不完整且有兜底工具。
TEST(SkillsIndexFormatTest, ExtremeOverflowCutsTailWithMarker) {
    std::vector<acecode::SkillMetadata> skills;
    for (int i = 0; i < 50; ++i) {
        skills.push_back(make_skill("very-long-skill-name-" + std::to_string(i),
                                    "desc"));
    }
    std::string out = acecode::format_skills_index_within_budget(skills, 150);
    EXPECT_LE(out.size(), 150u);
    EXPECT_NE(out.find("more skills"), std::string::npos);
    EXPECT_NE(out.find("skills_list"), std::string::npos);
}

// ---------------------------------------------------------------------------
// build_skills_index_context_prompt
// ---------------------------------------------------------------------------

// 触发场景:registry 为 nullptr(skill 功能未初始化)。
// 期望:空块 —— content/cache_key 都为空,session context 不包含 skill 段。
TEST(SkillsIndexBlockTest, NullRegistryYieldsEmptyBlock) {
    auto block = acecode::build_skills_index_context_prompt(nullptr, 128000);
    EXPECT_TRUE(block.content.empty());
    EXPECT_TRUE(block.cache_key.empty());
}

// 触发场景:registry 扫描了一个空目录(用户没装任何 skill)。
// 期望:空块,不发 "# Available Skills" 标题。
TEST_F(SkillsIndexRegistryTest, EmptyRegistryYieldsEmptyBlock) {
    acecode::SkillRegistry registry;
    registry.set_scan_roots({temp_root});
    registry.scan();
    auto block = acecode::build_skills_index_context_prompt(&registry, 128000);
    EXPECT_TRUE(block.content.empty());
}

// 触发场景:registry 有 skill(含 whenToUse)。
// 期望:块含 "# Available Skills" 标题、skill_view 指引、索引条目与触发条件;
// cache_key 非空且带 "skills:" 前缀(进 session context 复合 key)。
TEST_F(SkillsIndexRegistryTest, PopulatedRegistryRendersBlock) {
    write_skill_md(temp_root, "review", "review-pr", "Review pull requests",
                   "Use when the user mentions a PR");
    acecode::SkillRegistry registry;
    registry.set_scan_roots({temp_root});
    registry.scan();

    auto block = acecode::build_skills_index_context_prompt(&registry, 128000);
    EXPECT_NE(block.content.find("# Available Skills"), std::string::npos);
    EXPECT_NE(block.content.find("skill_view"), std::string::npos);
    EXPECT_NE(block.content.find("review-pr: Review pull requests"), std::string::npos);
    EXPECT_NE(block.content.find("Use when the user mentions a PR"), std::string::npos);
    EXPECT_EQ(block.cache_key.rfind("skills:", 0), 0u);
}

// 触发场景:同一 skill 集合构建两次 / 改动描述后再构建。
// 期望:集合不变 → cache_key 逐字节一致(cached_context_for_api 直接复用);
// 描述变化 → cache_key 变化(缓存失效,新索引上行)。
TEST_F(SkillsIndexRegistryTest, CacheKeyTracksSkillSetChanges) {
    write_skill_md(temp_root, "", "alpha", "First description");
    acecode::SkillRegistry registry;
    registry.set_scan_roots({temp_root});
    registry.scan();

    auto block1 = acecode::build_skills_index_context_prompt(&registry, 128000);
    auto block2 = acecode::build_skills_index_context_prompt(&registry, 128000);
    EXPECT_EQ(block1.cache_key, block2.cache_key);

    write_skill_md(temp_root, "", "alpha", "Changed description");
    registry.reload();
    auto block3 = acecode::build_skills_index_context_prompt(&registry, 128000);
    EXPECT_NE(block1.cache_key, block3.cache_key);
}

// ---------------------------------------------------------------------------
// build_session_context_prompt 集成
// ---------------------------------------------------------------------------

// 触发场景:带 skill registry 构建 session context(memory/project 均关闭)。
// 期望:skills 索引出现在 <system-reminder> 包裹内 —— 与 memory index、
// project instructions 同载体,每请求重建、不落盘、天然幂等。
TEST_F(SkillsIndexRegistryTest, SessionContextIncludesSkillsBlock) {
    write_skill_md(temp_root, "", "alpha", "First skill");
    acecode::SkillRegistry registry;
    registry.set_scan_roots({temp_root});
    registry.scan();

    auto block = acecode::build_session_context_prompt(
        temp_root.string(), nullptr, nullptr, nullptr, &registry, 128000);
    ASSERT_FALSE(block.content.empty());
    EXPECT_EQ(block.content.rfind("<system-reminder>", 0), 0u);
    EXPECT_NE(block.content.find("# Available Skills"), std::string::npos);
    EXPECT_NE(block.content.find("alpha: First skill"), std::string::npos);
    EXPECT_NE(block.content.find("</system-reminder>"), std::string::npos);
}

// 触发场景:不传 registry(旧调用方 / skill 功能关闭)。
// 期望:行为与改动前完全一致 —— 无其它 context 时返回空块。向后兼容回归。
TEST_F(SkillsIndexRegistryTest, SessionContextWithoutRegistryUnchanged) {
    auto block = acecode::build_session_context_prompt(
        temp_root.string(), nullptr, nullptr, nullptr);
    EXPECT_TRUE(block.content.empty());
}

// ---------------------------------------------------------------------------
// 静态 system prompt 措辞
// ---------------------------------------------------------------------------

// 触发场景:构建静态 system prompt。
// 期望:# Skills 段指向 system-reminder 内的索引("# Available Skills"),
// 含 "err on the side of loading" 强化措辞;不再把 skills_list 当作主发现
// 路径("Call `skills_list` to enumerate" 旧文案应删除)。
TEST(SkillsIndexSystemPromptTest, SkillsSectionPointsAtInContextIndex) {
    acecode::ToolExecutor tools;
    std::string prompt = acecode::build_system_prompt(tools, ".");

    EXPECT_NE(prompt.find("# Available Skills"), std::string::npos);
    EXPECT_NE(prompt.find("err on the side of loading"), std::string::npos);
    EXPECT_NE(prompt.find("BLOCKING REQUIREMENT"), std::string::npos);
    EXPECT_EQ(prompt.find("Call `skills_list` to enumerate"), std::string::npos);
}

// ---------------------------------------------------------------------------
// skill_loader: whenToUse frontmatter
// ---------------------------------------------------------------------------

// 触发场景:SKILL.md frontmatter 带 whenToUse(camelCase,claude-code 约定)。
// 期望:SkillMetadata.when_to_use 持有该值。
TEST_F(SkillsIndexRegistryTest, LoaderParsesWhenToUseCamelCase) {
    write_skill_md(temp_root, "", "alpha", "Desc", "Use for X");
    acecode::SkillRegistry registry;
    registry.set_scan_roots({temp_root});
    registry.scan();
    auto meta = registry.find("alpha");
    ASSERT_TRUE(meta.has_value());
    EXPECT_EQ(meta->when_to_use, "Use for X");
}

// 触发场景:frontmatter 用 snake_case 别名 when_to_use。
// 期望:同样被解析(两种写法等价,camelCase 优先)。
TEST_F(SkillsIndexRegistryTest, LoaderParsesWhenToUseSnakeCaseAlias) {
    fs::path dir = temp_root / "beta";
    fs::create_directories(dir);
    {
        std::ofstream ofs(dir / "SKILL.md", std::ios::binary);
        ofs << "---\n"
            << "name: beta\n"
            << "description: Desc\n"
            << "when_to_use: Use for Y\n"
            << "---\n\n# beta\n";
    }
    acecode::SkillRegistry registry;
    registry.set_scan_roots({temp_root});
    registry.scan();
    auto meta = registry.find("beta");
    ASSERT_TRUE(meta.has_value());
    EXPECT_EQ(meta->when_to_use, "Use for Y");
}

} // namespace
