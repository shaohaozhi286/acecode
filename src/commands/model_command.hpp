// /model slash command —— display the picker or switch the active model entry.
// Sub-actions:
//   /model                       — list saved_models + (legacy), mark current with '*'
//   /model <name>                — in-memory switch
//   /model --cwd <name>          — switch + persist to <cwd_hash>/model_override.json
//   /model --default <name>      — switch + persist to config.json default_model_name
//   /model add name=X provider=Y model=Z base_url=... api_key=...
//   /model edit <name> [field=value ...]
//   /model rm <name>
//   /model set-default <name>
#pragma once

#include "command_registry.hpp"

#include <map>
#include <string>

namespace acecode {

// 暴露给单测的纯解析函数。raw 是 /model 后面的字符串(已 trim)。
struct ParsedModelSub {
    std::string sub;   // "add" | "edit" | "rm" | "set-default" | ""
    std::string flag;  // "--cwd" | "--default" | ""(仅 sub=="" 时有效)
    std::string name;
    std::map<std::string, std::string> kvs;
};

// 解析 /model 后面的参数。返回 false 表示参数格式无效。
bool parse_model_subcommand(const std::string& raw, ParsedModelSub& out);

void register_model_command(CommandRegistry& registry);

} // namespace acecode
