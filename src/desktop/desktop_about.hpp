#pragma once

#include <string>

namespace acecode::desktop {

struct DesktopAboutInfo {
    std::string acecode_version;
    std::string browser_name;
    std::string browser_version;
    std::string compiler_version;
};

std::string format_msvc_compiler_version(int msc_version,
                                         long long msc_full_version);
std::string current_compiler_version();
std::string format_desktop_about_content(const DesktopAboutInfo& info);

// Windows: opens a Task Dialog parented to the desktop HWND and falls back to
// MessageBoxW if Task Dialog is unavailable. Other platforms return false so
// the web layer can use Settings > About as its fallback.
bool show_desktop_about_dialog(void* parent_window,
                               const DesktopAboutInfo& info);

} // namespace acecode::desktop
