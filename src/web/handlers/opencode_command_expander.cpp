#include "opencode_command_expander.hpp"

#include "../../commands/opencode_command.hpp"
#include "../../config/config.hpp"

namespace acecode::web {

OpencodeCommandExpansionResult try_expand_opencode_command(
    const std::string& original_text,
    const AppConfig& config,
    const std::string& workspace_cwd) {
    auto parsed = parse_opencode_slash_command(original_text);
    if (!parsed.has_value()) {
        return {false, original_text, ""};
    }
    if (is_web_reserved_builtin_command(parsed->name)) {
        return {false, original_text, parsed->name};
    }

    auto command = find_opencode_command(config, workspace_cwd, parsed->name);
    if (!command.has_value()) {
        return {false, original_text, parsed->name};
    }

    OpencodeCommandExpansionResult result;
    result.expanded = true;
    result.command_name = command->name;
    result.text = expand_opencode_command(*command, parsed->args);
    return result;
}

} // namespace acecode::web
