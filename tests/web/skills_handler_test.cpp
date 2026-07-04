// 覆盖 src/web/handlers/skills_handler.cpp。Skills 启停涉及 config 写盘 +
// SkillRegistry 重扫,一旦回归:
//   - 启停未持久化到 config.json → daemon 重启后状态回滚
//   - 不存在的 skill 没返 404 → cfg.skills.disabled 数组被污染垃圾名
//   - 重复 toggle 重复加入 disabled → 数组膨胀

#include <gtest/gtest.h>

#include "web/handlers/skills_handler.hpp"

#include "config/config.hpp"
#include "skills/skill_registry.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>

namespace fs = std::filesystem;

namespace {

void write_skill(const fs::path& root, const std::string& name,
                  const std::string& description) {
    fs::path dir = root / "engineering" / name;
    fs::create_directories(dir);
    std::ofstream ofs(dir / "SKILL.md", std::ios::binary);
    ofs << "---\n"
        << "name: " << name << "\n"
        << "description: " << description << "\n"
        << "---\n\n"
        << "# " << name << "\n\n"
        << description << "\n";
}

class SkillsHandlerTest : public ::testing::Test {
protected:
    fs::path tmp_root;
    std::string cfg_path;

    void SetUp() override {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        tmp_root = fs::temp_directory_path() /
                   ("acecode_skills_handler_test_" + std::to_string(gen()));
        fs::create_directories(tmp_root);
        cfg_path = (tmp_root / "config.json").string();
        // 写一个空 cfg
        std::ofstream(cfg_path) << "{}";
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tmp_root, ec);
    }
};

// SkillRegistry 包含 mutex 不可拷贝/移动 —— helper 通过 reference 填充 caller
// 持有的实例。
void populate_registry_with_skill(acecode::SkillRegistry& r,
                                    const fs::path& root,
                                    const std::string& skill_name) {
    write_skill(root, skill_name, "test description");
    r.set_scan_roots({root});
    r.scan();
}

} // namespace

// 场景: 把已注册 skill 禁用 → 200 + cfg.skills.disabled 含该 name + 文件回写
TEST_F(SkillsHandlerTest, DisableEnabledSkillUpdatesConfig) {
    acecode::SkillRegistry registry;
    populate_registry_with_skill(registry, tmp_root, "foo-skill");
    acecode::AppConfig cfg;

    auto r = acecode::web::set_skill_enabled("foo-skill", /*enabled=*/false,
                                                cfg, registry, cfg_path);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.http_status, 200);
    ASSERT_EQ(cfg.skills.disabled.size(), 1u);
    EXPECT_EQ(cfg.skills.disabled[0], "foo-skill");

    // 再次 disable 同一 name → 不应重复追加
    r = acecode::web::set_skill_enabled("foo-skill", /*enabled=*/false,
                                           cfg, registry, cfg_path);
    EXPECT_EQ(cfg.skills.disabled.size(), 1u);
}

// 场景: 重新启用一个已 disabled 的 skill → cfg 移除条目 + 200
TEST_F(SkillsHandlerTest, EnableDisabledSkillRemovesEntry) {
    acecode::SkillRegistry registry;
    populate_registry_with_skill(registry, tmp_root, "foo-skill");
    acecode::AppConfig cfg;
    cfg.skills.disabled = {"foo-skill"};
    // 让 registry 知道这个 skill 已 disabled(否则它会出现在 list 里)
    registry.set_disabled({"foo-skill"});
    registry.reload();

    auto r = acecode::web::set_skill_enabled("foo-skill", /*enabled=*/true,
                                                cfg, registry, cfg_path);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.http_status, 200);
    EXPECT_EQ(cfg.skills.disabled.size(), 0u);
}

// 场景: 不存在的 skill → 404 + 不污染 disabled 数组
TEST_F(SkillsHandlerTest, UnknownSkillReturns404) {
    acecode::SkillRegistry registry;
    populate_registry_with_skill(registry, tmp_root, "real-skill");
    acecode::AppConfig cfg;

    auto r = acecode::web::set_skill_enabled("nonexistent", /*enabled=*/false,
                                                cfg, registry, cfg_path);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.http_status, 404);
    EXPECT_TRUE(cfg.skills.disabled.empty());
}

// 场景: get_skill_body 返回 SKILL.md 完整内容(含 frontmatter)。
// SkillRegistry::read_skill_body 实现是去掉 frontmatter 的正文 — 该函数文档
// 写"含 frontmatter",这里检查至少返回非空。如果 read_skill_body 实现变化,
// 测试要跟着改。
TEST_F(SkillsHandlerTest, GetSkillBodyReturnsContent) {
    acecode::SkillRegistry registry;
    populate_registry_with_skill(registry, tmp_root, "doc-skill");
    auto body = acecode::web::get_skill_body("doc-skill", registry);
    ASSERT_TRUE(body.has_value());
    // body 至少包含 description 文本
    EXPECT_NE(body->find("test description"), std::string::npos);
}

// 场景: get_skill_body 不存在 → nullopt
TEST_F(SkillsHandlerTest, GetSkillBodyMissing) {
    acecode::SkillRegistry registry;
    populate_registry_with_skill(registry, tmp_root, "real");
    EXPECT_FALSE(acecode::web::get_skill_body("nonexistent", registry).has_value());
}

TEST_F(SkillsHandlerTest, SelectSkillRootPrefersAcecodeProjectSkills) {
    const auto project = tmp_root / "project";
    const auto local_skills = project / ".acecode" / "skills";
    const auto agent_skills = project / ".agent" / "skills";
    const auto global_skills = tmp_root / "global" / "skills";
    fs::create_directories(local_skills);
    fs::create_directories(agent_skills);

    auto selected = acecode::web::select_skill_root(project, global_skills, false);
    EXPECT_EQ(selected.source, "project_acecode");
    EXPECT_EQ(fs::weakly_canonical(selected.path), fs::weakly_canonical(local_skills));
}

TEST_F(SkillsHandlerTest, SelectSkillRootUsesAgentProjectSkillsWhenAcecodeMissing) {
    const auto project = tmp_root / "project";
    const auto agent_skills = project / ".agent" / "skills";
    const auto global_skills = tmp_root / "global" / "skills";
    fs::create_directories(agent_skills);

    auto selected = acecode::web::select_skill_root(project, global_skills, false);
    EXPECT_EQ(selected.source, "project_agent");
    EXPECT_EQ(fs::weakly_canonical(selected.path), fs::weakly_canonical(agent_skills));
}

TEST_F(SkillsHandlerTest, SelectSkillRootCreateModePreservesExistingAgentProjectSkills) {
    const auto project = tmp_root / "project";
    const auto local_skills = project / ".acecode" / "skills";
    const auto agent_skills = project / ".agent" / "skills";
    const auto global_skills = tmp_root / "global" / "skills";
    fs::create_directories(agent_skills);

    auto selected = acecode::web::select_skill_root(project, global_skills, true);
    EXPECT_EQ(selected.source, "project_agent");
    EXPECT_FALSE(fs::exists(local_skills));
    EXPECT_EQ(fs::weakly_canonical(selected.path), fs::weakly_canonical(agent_skills));
}

TEST_F(SkillsHandlerTest, SelectSkillRootCreatesWorkspaceAcecodeSkillsBeforeGlobalFallback) {
    const auto project = tmp_root / "project";
    const auto local_skills = project / ".acecode" / "skills";
    const auto global_skills = tmp_root / "global" / "skills";
    fs::create_directories(project);

    auto selected = acecode::web::select_skill_root(project, global_skills, true);
    EXPECT_EQ(selected.source, "project_acecode");
    EXPECT_TRUE(fs::is_directory(local_skills));
    EXPECT_EQ(fs::weakly_canonical(selected.path), fs::weakly_canonical(local_skills));
}

TEST_F(SkillsHandlerTest, SelectSkillRootFallsBackToGlobalAcecodeSkillsAndCreatesItWithoutWorkspace) {
    const auto global_skills = tmp_root / "global" / "skills";

    auto selected = acecode::web::select_skill_root({}, global_skills, true);
    EXPECT_EQ(selected.source, "global_acecode");
    EXPECT_TRUE(fs::is_directory(global_skills));
    EXPECT_EQ(fs::weakly_canonical(selected.path), fs::weakly_canonical(global_skills));
}

// 场景: select_skill_root 无论选中项目根还是全局根,global_path 都指向
// 传入的全局根 —— 设置页「打开全局 Skill 目录」按钮依赖这个字段,一旦
// 回归成空/项目路径,按钮会打开错误目录。
TEST_F(SkillsHandlerTest, SelectSkillRootAlwaysReportsGlobalPath) {
    const auto project = tmp_root / "project";
    const auto local_skills = project / ".acecode" / "skills";
    const auto global_skills = tmp_root / "global" / "skills";
    fs::create_directories(local_skills);

    auto selected = acecode::web::select_skill_root(project, global_skills, false);
    EXPECT_EQ(selected.source, "project_acecode");
    EXPECT_EQ(fs::weakly_canonical(selected.global_path),
              fs::weakly_canonical(global_skills));

    auto fallback = acecode::web::select_skill_root({}, global_skills, false);
    EXPECT_EQ(fallback.source, "global_acecode");
    EXPECT_EQ(fs::weakly_canonical(fallback.global_path),
              fs::weakly_canonical(global_skills));
}

// ─── build_skills_payload_with_roots ────────────────────────────────────────

namespace {

// 在 payload 数组里按 name 找条目;找不到返回 nullptr。
const nlohmann::json* find_entry(const nlohmann::json& arr, const std::string& name) {
    for (const auto& o : arr) {
        if (o.value("name", "") == name) return &o;
    }
    return nullptr;
}

} // namespace

// 场景: 项目根与全局根各有一个 skill → source 分别标记 "project"/"global",
// 未禁用时 enabled=true。设置页「本地/全局分组」依赖 source 字段,一旦
// 回归(全部标成 global)项目技能会掉进全局列表。
TEST_F(SkillsHandlerTest, BuildSkillsPayloadClassifiesProjectAndGlobalSources) {
    const auto project_root = tmp_root / "proj" / ".acecode" / "skills";
    const auto global_root  = tmp_root / "home" / ".acecode" / "skills";
    write_skill(project_root, "proj-skill", "project scoped");
    write_skill(global_root, "glob-skill", "globally scoped");

    auto arr = acecode::web::build_skills_payload_with_roots(
        {project_root}, {global_root}, /*disabled=*/{});

    const auto* proj = find_entry(arr, "proj-skill");
    ASSERT_NE(proj, nullptr);
    EXPECT_EQ((*proj)["source"], "project");
    EXPECT_TRUE((*proj)["enabled"].get<bool>());
    EXPECT_EQ((*proj)["description"], "project scoped");

    const auto* glob = find_entry(arr, "glob-skill");
    ASSERT_NE(glob, nullptr);
    EXPECT_EQ((*glob)["source"], "global");
    EXPECT_TRUE((*glob)["enabled"].get<bool>());
}

// 场景: 已禁用的 skill 磁盘上仍存在 → enabled=false 但保留完整
// description/source。回归表现(旧实现):禁用条目只剩名字,设置页里
// 描述变成空、分组信息丢失。
TEST_F(SkillsHandlerTest, BuildSkillsPayloadKeepsMetadataForDisabledSkills) {
    const auto global_root = tmp_root / "home" / ".acecode" / "skills";
    write_skill(global_root, "off-skill", "still has description");

    auto arr = acecode::web::build_skills_payload_with_roots(
        {}, {global_root}, /*disabled=*/{"off-skill"});

    const auto* off = find_entry(arr, "off-skill");
    ASSERT_NE(off, nullptr);
    EXPECT_FALSE((*off)["enabled"].get<bool>());
    EXPECT_EQ((*off)["description"], "still has description");
    EXPECT_EQ((*off)["source"], "global");
}

// 场景: disabled 列表里残留一个磁盘上已删除的名字(幽灵条目)→ 仍列出
// (enabled=false、source=""),这样用户能在 UI 里把它从禁用列表放出来,
// 而不是永远卡在 config.json 里。
TEST_F(SkillsHandlerTest, BuildSkillsPayloadListsGhostDisabledEntries) {
    const auto global_root = tmp_root / "home" / ".acecode" / "skills";
    write_skill(global_root, "real-skill", "exists on disk");

    auto arr = acecode::web::build_skills_payload_with_roots(
        {}, {global_root}, /*disabled=*/{"deleted-skill"});

    const auto* ghost = find_entry(arr, "deleted-skill");
    ASSERT_NE(ghost, nullptr);
    EXPECT_FALSE((*ghost)["enabled"].get<bool>());
    EXPECT_EQ((*ghost)["source"], "");
    EXPECT_EQ((*ghost)["description"], "");
}

// 场景: 项目根与全局根有同名 skill → first-wins(项目优先),payload 里只
// 出现一次且 source="project"。与 SkillRegistry 的去重语义保持一致;一旦
// 回归成出现两次,设置页 toggle 会同时打到两行。
TEST_F(SkillsHandlerTest, BuildSkillsPayloadDeduplicatesByNameProjectWins) {
    const auto project_root = tmp_root / "proj" / ".acecode" / "skills";
    const auto global_root  = tmp_root / "home" / ".acecode" / "skills";
    write_skill(project_root, "same-skill", "project copy");
    write_skill(global_root, "same-skill", "global copy");

    auto arr = acecode::web::build_skills_payload_with_roots(
        {project_root}, {global_root}, /*disabled=*/{});

    int count = 0;
    for (const auto& o : arr) {
        if (o.value("name", "") == "same-skill") ++count;
    }
    EXPECT_EQ(count, 1);
    const auto* entry = find_entry(arr, "same-skill");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ((*entry)["source"], "project");
    EXPECT_EQ((*entry)["description"], "project copy");
}
