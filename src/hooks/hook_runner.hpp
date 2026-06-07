#pragma once

#include "hook_config.hpp"

#include <chrono>
#include <string>

namespace acecode {

struct HookProcessResult {
    bool started = false;
    bool timed_out = false;
    int exit_code = -1;
    long long duration_ms = 0;
    std::string output;
    std::string error;
};

std::string resolve_hook_command_path(const std::string& command);

HookProcessResult run_hook_process(const HookCommandSpec& command,
                                   const std::string& stdin_text,
                                   int timeout_ms,
                                   const std::string& cwd);

} // namespace acecode
