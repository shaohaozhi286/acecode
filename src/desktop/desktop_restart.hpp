#pragma once

#include <filesystem>
#include <string>

namespace acecode::desktop {

struct DesktopRestartPreflight {
    bool ok = false;
    std::string error;
};

// Resolve this process's installed executable path. Call this before an update
// can rename the running image into its backup directory.
std::filesystem::path current_desktop_executable_path();

// Check the immutable install path before accepting a restart request.
DesktopRestartPreflight validate_desktop_restart_target(
    const std::filesystem::path& executable);

// Launch the replacement desktop process. The caller must first tear down its
// daemon/tray resources and release the desktop single-instance guard.
bool launch_desktop_replacement(const std::filesystem::path& executable,
                                std::string* error);

} // namespace acecode::desktop
