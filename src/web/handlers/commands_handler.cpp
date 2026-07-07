#include "commands_handler.hpp"

#include "../../commands/opencode_command.hpp"
#include "../../config/config.hpp"
#include "../../skills/skill_init.hpp"
#include "../../skills/skill_registry.hpp"
#include "../../utils/encoding.hpp"

#include <algorithm>
#include <unordered_set>

namespace acecode::web {

namespace {

// 把 SkillRegistry 的 list() 结果合并到 collected,按 name 去重(first-wins)。
void merge_skills(std::vector<SkillMetadata>& collected,
                  std::unordered_set<std::string>& seen,
                  const std::vector<SkillMetadata>& src) {
    for (const auto& s : src) {
        if (s.name.empty() || seen.count(s.name)) continue;
        seen.insert(s.name);
        collected.push_back(s);
    }
}

} // namespace

nlohmann::json build_commands_payload(const SkillRegistry& global_skills,
                                        const std::optional<std::string>& workspace_cwd,
                                        const AppConfig* cfg) {
    nlohmann::json out;

    // Builtin 白名单:与 src/commands/init_command.cpp / builtin_commands.cpp
    // 中的描述保持一致。新增/修改命令描述时 grep "init_command" 与 "compact" 同步。
    nlohmann::json builtins = nlohmann::json::array();
    builtins.push_back({
        {"name", "init"},
        {"description", "Analyze this codebase and generate (or improve) AGENT.md"},
    });
    builtins.push_back({
        {"name", "compact"},
        {"description", "Compress conversation history"},
    });
    builtins.push_back({
        {"name", "goal"},
        {"description", "Create, view, pause, resume, edit, or clear the thread goal"},
    });
    builtins.push_back({
        {"name", "plan"},
        {"description", "Enter plan mode or start planning a described task"},
    });
    builtins.push_back({
        {"name", "lsp"},
        {"description", "Show LSP server status (connected/broken/not installed)"},
    });
    out["builtins"] = std::move(builtins);

    // Skills:仅在调用方传 workspace_cwd 时返回 `skills` 字段;缺省 → 不输出
    // 该字段(向后兼容 add-webui-slash-commands v1)。这样旧客户端零变化,新
    // 客户端拿到 workspace 定向 skills。
    if (workspace_cwd && !workspace_cwd->empty() && cfg) {
        auto commands = load_opencode_commands(*cfg, *workspace_cwd);
        std::sort(commands.begin(), commands.end(),
                  [](const OpencodeCommandInfo& a, const OpencodeCommandInfo& b) {
                      return a.name < b.name;
                  });
        nlohmann::json command_arr = nlohmann::json::array();
        for (const auto& c : commands) {
            if (is_web_reserved_builtin_command(c.name)) continue;
            command_arr.push_back({
                {"name", ensure_utf8(c.name)},
                {"description", ensure_utf8(c.description)},
                {"subtask", c.subtask},
            });
        }
        out["commands"] = std::move(command_arr);

        std::vector<SkillMetadata> entries;
        std::unordered_set<std::string> seen;

        SkillRegistry ws_reg;
        initialize_skill_registry(ws_reg, *cfg, *workspace_cwd);
        merge_skills(entries, seen, ws_reg.list());
        merge_skills(entries, seen, global_skills.list());

        std::sort(entries.begin(), entries.end(),
                  [](const SkillMetadata& a, const SkillMetadata& b) {
                      return a.name < b.name;
                  });
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& s : entries) {
            arr.push_back({
                {"name", ensure_utf8(s.name)},
                {"description", ensure_utf8(s.description)},
            });
        }
        out["skills"] = std::move(arr);
    }

    return out;
}

} // namespace acecode::web
