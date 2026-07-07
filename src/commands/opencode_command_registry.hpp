#pragma once

#include <string>
#include <vector>

namespace acecode {

class CommandRegistry;
struct AppConfig;

std::vector<std::string> register_opencode_commands(
    CommandRegistry& cmd_registry,
    const AppConfig& config,
    const std::string& working_dir);

void unregister_opencode_commands(
    CommandRegistry& cmd_registry,
    const std::vector<std::string>& command_keys);

std::vector<std::string> register_opencode_commands_tracked(
    CommandRegistry& cmd_registry,
    const AppConfig& config,
    const std::string& working_dir);

std::size_t reload_opencode_commands(
    CommandRegistry& cmd_registry,
    const AppConfig& config,
    const std::string& working_dir);

} // namespace acecode
