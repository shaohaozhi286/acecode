#pragma once

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
using DesktopLauncher = std::function<DesktopLaunchResult()>;

// Resolve the flat-layout desktop binary beside the running acecode binary.
std::filesystem::path sibling_desktop_executable(
    const std::filesystem::path& acecode_executable);

// Validate and launch a desktop sibling through an injectable detached-process
// seam. The spawner receives only argv[0]; /desktop adds no command arguments.
DesktopLaunchResult launch_sibling_desktop(
    const std::filesystem::path& acecode_executable,
    const DesktopProcessSpawner& spawn_process);

// Production launcher: resolve the current process through the native daemon
// platform layer, then start its desktop sibling detached.
DesktopLaunchResult launch_sibling_desktop();

// Register /desktop. Tests may inject a launcher to avoid creating a process.
void register_desktop_command(CommandRegistry& registry,
                              DesktopLauncher launcher = {});

} // namespace acecode
