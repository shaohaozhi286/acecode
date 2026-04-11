#include "system_prompt.hpp"
#include <sstream>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#endif

namespace acecode {

static std::string get_os_name() {
#ifdef _WIN32
    return "Windows";
#elif __APPLE__
    return "macOS";
#else
    return "Linux";
#endif
}

static std::string get_default_shell() {
#ifdef _WIN32
    return "cmd.exe";
#else
    const char* shell = std::getenv("SHELL");
    return shell ? shell : "/bin/sh";
#endif
}

// Generate tool descriptions from registered ToolDefs
static std::string generate_tools_prompt(const ToolExecutor& tools) {
    auto defs = tools.get_tool_definitions();
    if (defs.empty()) return "";

    std::ostringstream oss;
    oss << "# Tools\n\n"
        << "You have access to the following tools:\n\n";

    for (const auto& def : defs) {
        oss << "## " << def.name << "\n"
            << "Description: " << def.description << "\n"
            << "Parameters:\n```json\n"
            << def.parameters.dump(2) << "\n```\n\n";
    }

    return oss.str();
}

std::string build_system_prompt(const ToolExecutor& tools, const std::string& cwd) {
    std::ostringstream oss;

    // Identity
    oss << "You are an expert AI programming assistant called acecode. "
        << "You help users read, write, and edit code in their projects.\n\n";

    // Environment
    oss << "# Environment\n\n"
        << "- OS: " << get_os_name() << "\n"
        << "- CWD: " << cwd << "\n"
        << "- Shell: " << get_default_shell() << "\n\n";

    // Tools
    oss << generate_tools_prompt(tools);

    // Behavior rules
    oss << "# Rules\n\n"
        << "- Always use absolute file paths when calling file tools.\n"
        << "- Before editing a file, read it first to understand its content.\n"
        << "- When using file_edit, include enough context lines in old_string to uniquely identify the target.\n"
        << "- When using bash, prefer simple commands. Avoid interactive programs.\n"
        << "- If a tool call fails, analyze the error and try a different approach.\n"
        << "- Be concise in your responses. Focus on the task at hand.\n";

    return oss.str();
}

} // namespace acecode
