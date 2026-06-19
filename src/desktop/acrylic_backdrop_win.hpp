#pragma once

#ifdef _WIN32

#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>

#  include <string>

#  define ACECODE_DESKTOP_ACRYLIC_BACKEND_AUTO 0
#  define ACECODE_DESKTOP_ACRYLIC_BACKEND_SYSTEM 1
#  define ACECODE_DESKTOP_ACRYLIC_BACKEND_DEMO 2
#  define ACECODE_DESKTOP_ACRYLIC_BACKEND_OFF 3
#  define ACECODE_DESKTOP_ACRYLIC_BACKEND_ACCENT 4

#  ifndef ACECODE_DESKTOP_ACRYLIC_BACKEND
#    define ACECODE_DESKTOP_ACRYLIC_BACKEND ACECODE_DESKTOP_ACRYLIC_BACKEND_AUTO
#  endif
#  ifndef ACECODE_DESKTOP_ACRYLIC_USE_WEBVIEW_OWNED_WINDOW
#    define ACECODE_DESKTOP_ACRYLIC_USE_WEBVIEW_OWNED_WINDOW 1
#  endif
#  ifndef ACECODE_DESKTOP_ACRYLIC_USE_FRAMELESS_POPUP
#    define ACECODE_DESKTOP_ACRYLIC_USE_FRAMELESS_POPUP 0
#  endif
#  ifndef ACECODE_DESKTOP_ACRYLIC_USE_LAYERED_COLORKEY
#    define ACECODE_DESKTOP_ACRYLIC_USE_LAYERED_COLORKEY 0
#  endif

namespace acecode::desktop {

enum class DesktopAcrylicBackend {
    Off,
    Accent,
    System,
    Demo,
};

struct DesktopAcrylicStatus {
    bool enabled = false;
    DesktopAcrylicBackend backend = DesktopAcrylicBackend::Off;
    std::string error;
};

struct DesktopAcrylicOptions {
    int tint_red = 221;
    int tint_green = 221;
    int tint_blue = 221;
    int tint_alpha = 136;
};

DesktopAcrylicStatus enable_desktop_sidebar_acrylic(HWND hwnd);
void handle_desktop_sidebar_acrylic_message(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
bool set_webview_default_background_transparent(void* browser_controller, std::string* error);
const char* desktop_acrylic_backend_name(DesktopAcrylicBackend backend);
bool desktop_sidebar_acrylic_prefers_webview_owned_window();
bool desktop_sidebar_acrylic_uses_layered_colorkey();
DesktopAcrylicOptions desktop_sidebar_acrylic_options();
void set_desktop_sidebar_acrylic_options(const DesktopAcrylicOptions& options);

} // namespace acecode::desktop

#endif
