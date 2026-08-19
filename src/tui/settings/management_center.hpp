#pragma once

#include "settings_state.hpp"

#include "../../config/config.hpp"
#include "../../skills/skill_usage_store.hpp"

#include <ftxui/component/component_base.hpp>

#include <functional>
#include <memory>
#include <string>

namespace acecode {
class CommandRegistry;
class HookManager;
class McpManager;
class SkillRegistry;
class ToolExecutor;
}

namespace acecode::tui::settings {

struct ManagementCenterDependencies {
    AppConfig* config = nullptr;
    SkillRegistry* skills = nullptr;
    CommandRegistry* commands = nullptr;
    McpManager* mcp = nullptr;
    ToolExecutor* tools = nullptr;
    HookManager* hooks = nullptr;
    SkillUsageStore* skill_usage = nullptr;
    std::string cwd;
    std::function<void()> request_close;
    std::function<void()> post_event;
    std::function<void(std::function<void()>)> post_to_ui;
};

class ManagementCenter {
public:
    explicit ManagementCenter(ManagementCenterDependencies dependencies);
    ~ManagementCenter();

    ManagementCenter(const ManagementCenter&) = delete;
    ManagementCenter& operator=(const ManagementCenter&) = delete;

    ftxui::Component component() const;
    void open(ManagementTab tab = ManagementTab::Skills);
    ManagementTab active_tab() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace acecode::tui::settings
