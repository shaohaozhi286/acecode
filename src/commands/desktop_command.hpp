#pragma once

#include "../desktop/open_request.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace acecode {

class CommandRegistry;

struct DesktopLaunchResult {
    bool ok = false;
    std::filesystem::path executable;
    std::string error;
};

using DesktopProcessSpawner =
    std::function<bool(const std::vector<std::string>& argv)>;
using DesktopLauncher = std::function<DesktopLaunchResult(
    const desktop::DesktopOpenRequest& request)>;

// Resolve the flat-layout desktop binary beside the running acecode binary.
std::filesystem::path sibling_desktop_executable(
    const std::filesystem::path& acecode_executable);

// Validate and launch a desktop sibling through an injectable detached-process
// seam. The spawner receives argv[0] followed by the current TUI workspace and
// session as separate arguments.
DesktopLaunchResult launch_sibling_desktop(
    const std::filesystem::path& acecode_executable,
    const desktop::DesktopOpenRequest& request,
    const DesktopProcessSpawner& spawn_process);

// Production launcher: resolve the current process through the native daemon
// platform layer, then start its desktop sibling detached.
DesktopLaunchResult launch_sibling_desktop(
    const desktop::DesktopOpenRequest& request);

// Register /desktop. Tests may inject a launcher to avoid creating a process.
void register_desktop_command(CommandRegistry& registry,
                              DesktopLauncher launcher = {});

} // namespace acecode
