#pragma once

// 共享 SkillRegistry 初始化逻辑:扫描根目录顺序与 main.cpp 历史实现保持一致,
// 让 TUI(main.cpp)与 daemon(worker.cpp)看到同一组 skills。
//
// 扫描根顺序(first-wins by skill name):
//   1) project walk — <cwd...>/.acecode/skills,深度优先回溯到 HOME
//   2) project walk — <cwd...>/.agent/skills,深度优先回溯到 HOME
//   3) user global  — ~/.acecode/skills(自动创建)
//   4) user global  — ~/.agent/skills(兼容根)
//   5) external dirs — config.skills.external_dirs

#include <string>

namespace acecode {

class SkillRegistry;
struct AppConfig;

void initialize_skill_registry(SkillRegistry& skill_registry,
                                 const AppConfig& config,
                                 const std::string& working_dir);

} // namespace acecode
