#pragma once

// 共享 SkillRegistry 初始化逻辑:扫描根目录顺序与 main.cpp 历史实现保持一致,
// 让 TUI(main.cpp)与 daemon(worker.cpp)看到同一组 skills。
//
// 扫描根顺序(first-wins by skill name):
//   1) project walk — <cwd...>/.acecode/skills,深度优先回溯到 HOME
//   2) project walk — <cwd...>/.agent/skills,深度优先回溯到 HOME
//   3) opencode project compat roots — when config.skills.reuse_opencode is true
//   4) user global  — ~/.acecode/skills(自动创建)
//   5) user global  — ~/.agent/skills(兼容根)
//   6) opencode global/cache compat roots — when config.skills.reuse_opencode is true
//   7) external dirs — config.skills.external_dirs

#include <filesystem>
#include <string>
#include <vector>

namespace acecode {

class SkillRegistry;
struct AppConfig;

void initialize_skill_registry(SkillRegistry& skill_registry,
                                 const AppConfig& config,
                                 const std::string& working_dir);

// 项目链扫描根(上表 1-3 段):working_dir 沿目录树向上(不含 HOME 本身)。
// 命中这些根的 skill 在 web UI 里归类为「项目技能」。纯枚举,不创建目录。
std::vector<std::filesystem::path> project_skill_scan_roots(
    const AppConfig& config, const std::string& working_dir);

// 全局扫描根(上表 4-7 段):用户全局目录 + opencode 全局兼容根 + external_dirs。
// 纯枚举,不创建目录(~/.acecode/skills 的自动创建留在 initialize_skill_registry)。
std::vector<std::filesystem::path> global_skill_scan_roots(const AppConfig& config);

} // namespace acecode
