#pragma once

#include <string>

namespace acecode {
struct AppConfig;
}

namespace acecode::web {

struct OpencodeCommandExpansionResult {
    bool expanded = false;
    std::string text;
    std::string command_name;
};

OpencodeCommandExpansionResult try_expand_opencode_command(
    const std::string& original_text,
    const AppConfig& config,
    const std::string& workspace_cwd);

} // namespace acecode::web
