#pragma once

// Final Windows fallback for acecode-desktop startup. If embedded WebView2
// cannot be initialized, the desktop process can keep the supervised daemon
// alive and show the daemon web UI through Microsoft Edge app mode.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace acecode::desktop {

struct EdgeAppLaunchResult {
    bool ok = false;
    std::string error;
    unsigned long exit_code = 0;
};

// Pure helper used by unit tests. Each root is expected to be a ProgramFiles-like
// directory; the function checks Microsoft/Edge/Application/msedge.exe.
std::optional<std::filesystem::path> find_msedge_executable_in(
    const std::vector<std::filesystem::path>& roots);

// Windows system lookup. POSIX builds return nullopt.
std::optional<std::filesystem::path> find_msedge_executable();

// Build ShellExecute/CreateProcess-style parameters for Edge app mode. Exposed
// for tests because quoting the user-data-dir path is easy to regress.
std::wstring build_edge_app_parameters_w(const std::wstring& url,
                                         const std::wstring& user_data_dir);

// Launch Edge in app mode, wait for the app process to exit, and return its
// exit code. The caller remains responsible for daemon cleanup.
EdgeAppLaunchResult launch_edge_app_and_wait(const std::string& url);

} // namespace acecode::desktop
