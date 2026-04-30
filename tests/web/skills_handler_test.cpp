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
