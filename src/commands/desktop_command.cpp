#include "desktop_command.hpp"

#include "command_registry.hpp"
#include "../daemon/platform.hpp"
#include "../utils/utf8_path.hpp"

#include <cctype>
#include <exception>
#include <mutex>
#include <system_error>
#include <utility>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace acecode {
namespace {

bool has_non_whitespace(const std::string& text) {
    for (unsigned char ch : text) {
        if (std::isspace(ch) == 0) return true;
    }
    return false;
}

std::string display_path(const fs::path& path) {
    return path_to_utf8(path);
}

void emit_desktop_command_message(CommandContext& ctx, std::string message) {
    {
        std::lock_guard<std::mutex> lock(ctx.state.mu);
        ctx.state.conversation.push_back({"system", std::move(message), false});
        ctx.state.chat_follow_tail = true;
    }
    if (ctx.post_event) ctx.post_event();
}

} // namespace

fs::path sibling_desktop_executable(const fs::path& acecode_executable) {
#ifdef _WIN32
    constexpr const char* kDesktopExecutableName = "acecode-desktop.exe";
#else
    constexpr const char* kDesktopExecutableName = "acecode-desktop";
#endif
    if (acecode_executable.empty()) return {};
    return acecode_executable.parent_path() / kDesktopExecutableName;
}

DesktopLaunchResult launch_sibling_desktop(
    const fs::path& acecode_executable,
    const DesktopProcessSpawner& spawn_process) {
    DesktopLaunchResult result;
    if (acecode_executable.empty()) {
        result.error = "cannot resolve the running acecode executable path";
        return result;
    }

    result.executable = sibling_desktop_executable(acecode_executable);
    std::error_code ec;
    const bool exists = fs::exists(result.executable, ec);
    if (ec) {
        result.error = "failed to inspect desktop executable " +
                       display_path(result.executable) + ": " + ec.message();
        return result;
    }
    if (!exists) {
        result.error = "desktop executable does not exist: " +
                       display_path(result.executable);
        return result;
    }
    const bool regular_file = fs::is_regular_file(result.executable, ec);
    if (ec) {
        result.error = "failed to inspect desktop executable " +
                       display_path(result.executable) + ": " + ec.message();
        return result;
    }
    if (!regular_file) {
        result.error = "desktop executable is not a regular file: " +
                       display_path(result.executable);
        return result;
    }
#ifndef _WIN32
    if (::access(result.executable.c_str(), X_OK) != 0) {
        result.error = "desktop executable is not executable: " +
                       display_path(result.executable);
        return result;
    }
#endif
    if (!spawn_process) {
        result.error = "desktop process launcher is unavailable";
        return result;
    }

    const std::vector<std::string> argv{result.executable.string()};
    if (!spawn_process(argv)) {
        result.error = "failed to create detached desktop process: " +
                       display_path(result.executable);
        return result;
    }

    result.ok = true;
    return result;
}

DesktopLaunchResult launch_sibling_desktop() {
    const std::string current_executable = daemon::current_executable_path();
    return launch_sibling_desktop(
        fs::path(current_executable),
        [](const std::vector<std::string>& argv) {
            return daemon::spawn_detached(argv) != 0;
        });
}

void register_desktop_command(CommandRegistry& registry,
                              DesktopLauncher launcher) {
    if (!launcher) {
        launcher = [] { return launch_sibling_desktop(); };
    }

    registry.register_command({
        "desktop",
        "Open ACECode Desktop",
        [launcher = std::move(launcher)](CommandContext& ctx,
                                         const std::string& args) {
            if (has_non_whitespace(args)) {
                emit_desktop_command_message(ctx, "Usage: /desktop");
                return;
            }

            DesktopLaunchResult result;
            try {
                result = launcher();
            } catch (const std::exception& error) {
                result.error = error.what();
            } catch (...) {
                result.error = "unknown launch error";
            }

            if (result.ok) {
                emit_desktop_command_message(
                    ctx,
                    "ACECode Desktop started: " + display_path(result.executable));
                return;
            }

            emit_desktop_command_message(
                ctx,
                "Failed to start ACECode Desktop: " +
                    (result.error.empty() ? std::string("unknown launch error")
                                          : result.error));
        },
    });
}

} // namespace acecode
