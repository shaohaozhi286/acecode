// 覆盖 src/skills/skill_commands.cpp::reload_skill_commands 在「会话进行中磁盘上
// 新增/删除 skill」时的重绑行为。
//
// 背景:用户(或 agent 用 skill-creator)在 ACECode 运行期间往扫描根里写了个新
//   SKILL.md。SkillRegistry 本身 scan-on-read 是热的,但 TUI 的 CommandRegistry
//   是启动时一次性快照的 std::map —— 新 skill 不重绑就不在 /<name> 命令 + 自动
//   补全里。CommandRegistry::dispatch 现在对未命中命令先调 reload_skill_commands
//   重扫+重绑再查一次,这个文件锁定该 helper 的核心契约。
//
// 本文件覆盖的场景:
//   1. 启动时无该 skill → 命令缺席;磁盘写入后 reload → /<name> 命令出现(回归
//      "新 skill 必须重启" 的核心修复)。
//   2. reload 只动 skill key:同时存在的内建命令在 reload 后依然在,不被误删。
//   3. 磁盘上删掉 skill 后 reload → 对应命令被注销(不残留幽灵命令)。

#include <gtest/gtest.h>

#include "commands/command_registry.hpp"
#include "config/config.hpp"
#include "skills/skill_commands.hpp"
#include "skills/skill_init.hpp"
#include "skills/skill_registry.hpp"

#include <filesystem>
#include <fstream>
#include <random>

namespace fs = std::filesystem;

namespace {

void write_skill_md(const fs::path& skills_root, const std::string& name,
                    const std::string& description) {
    fs::path dir = skills_root / "general" / name;
    fs::create_directories(dir);
    std::ofstream ofs(dir / "SKILL.md", std::ios::binary);
    ofs << "---\n"
        << "name: " << name << "\n"
        << "description: " << description << "\n"
        << "---\n\nBody.\n";
}

class SkillCommandsReloadTest : public ::testing::Test {
protected:
    fs::path tmp_root;
    acecode::SkillRegistry skill_registry;
    acecode::CommandRegistry cmd_registry;
    acecode::AppConfig cfg;

    void SetUp() override {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        tmp_root = fs::temp_directory_path() /
                   ("acecode_skill_reload_test_" + std::to_string(gen()));
        fs::create_directories(agent_skills());
        // 关键:只把临时目录设为唯一扫描根,绕开 initialize_skill_registry
        // 会附带的真实 ~/.acecode/skills / ~/.agent/skills(那里有种子 skill,
        // 会污染断言)。
        skill_registry.set_scan_roots({agent_skills()});
        skill_registry.scan();
        // 模拟启动:把当前(空)skill 集合注册并记进 tracked 集合,
        // 这样后续 reload 能正确卸掉上一批。
        acecode::register_skill_commands_tracked(cmd_registry, skill_registry);
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tmp_root, ec);
    }

    fs::path agent_skills() const { return tmp_root / ".agent" / "skills"; }
};

} // namespace

// 场景 1:启动后磁盘写入新 skill,reload 让 /<name> 命令出现。
TEST_F(SkillCommandsReloadTest, NewSkillBecomesCommandAfterReload) {
    EXPECT_FALSE(cmd_registry.has_command("calculator"))
        << "启动时磁盘上还没有该 skill,命令不应存在";

    write_skill_md(agent_skills(), "calculator", "Simple calc skill");
    acecode::reload_skill_commands(cmd_registry, skill_registry);

    EXPECT_TRUE(cmd_registry.has_command("calculator"))
        << "磁盘写入后 reload 应注册 /calculator,无需重启";
}

// 场景 2:reload 只重绑 skill key,不误删同时存在的内建命令。
TEST_F(SkillCommandsReloadTest, ReloadPreservesNonSkillCommands) {
    acecode::SlashCommand builtin;
    builtin.name = "help";
    builtin.description = "builtin";
    builtin.execute = [](acecode::CommandContext&, const std::string&) {};
    cmd_registry.register_command(builtin);

    write_skill_md(agent_skills(), "calculator", "Simple calc skill");
    acecode::reload_skill_commands(cmd_registry, skill_registry);

    EXPECT_TRUE(cmd_registry.has_command("help"))
        << "内建命令不在 tracked skill key 集合里,reload 不应动它";
    EXPECT_TRUE(cmd_registry.has_command("calculator"));
}

// 场景 3:磁盘上删掉 skill 后 reload 注销对应命令,不留幽灵命令。
TEST_F(SkillCommandsReloadTest, RemovedSkillIsUnregisteredAfterReload) {
    write_skill_md(agent_skills(), "calculator", "Simple calc skill");
    acecode::reload_skill_commands(cmd_registry, skill_registry);
    ASSERT_TRUE(cmd_registry.has_command("calculator"));

    std::error_code ec;
    fs::remove_all(agent_skills() / "general" / "calculator", ec);
    acecode::reload_skill_commands(cmd_registry, skill_registry);

    EXPECT_FALSE(cmd_registry.has_command("calculator"))
        << "磁盘上 skill 已删,reload 后命令应被注销";
}
