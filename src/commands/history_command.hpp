// /history slash command —— 查看与清空当前工作目录的持久化输入历史。
// 详见 openspec/changes/persistent-input-history/spec.md Requirement "/history Slash Command".
#pragma once

#include "command_registry.hpp"

namespace acecode {

// Register the `/history` built-in command. Two sub-actions:
//   /history            — list current in-memory input history (oldest-first)
//   /history list       — alias of the above
//   /history clear      — wipe both in-memory queue and persistent file
void register_history_command(CommandRegistry& registry);

} // namespace acecode
