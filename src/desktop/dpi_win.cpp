#include "dpi_win.hpp"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>

namespace acecode::desktop {

bool enable_desktop_dpi_awareness() {
    // The desktop shell owns the Win32 parent window that hosts WebView2.
    // webview/webview enables DPI awareness only for windows it creates itself,
    // so the custom parent path must set this before any HWND is created.
    HMODULE user32 = ::LoadLibraryW(L"user32.dll");
    if (user32) {
        using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(HANDLE);
        auto set_context = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
            ::GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (set_context) {
            constexpr INT_PTR kPerMonitorAware = -3;
            constexpr INT_PTR kPerMonitorAwareV2 = -4;
            if (set_context(reinterpret_cast<HANDLE>(kPerMonitorAwareV2))) {
                ::FreeLibrary(user32);
                return true;
            }
            if (::GetLastError() == ERROR_ACCESS_DENIED) {
                ::FreeLibrary(user32);
                return true;
            }
            if (set_context(reinterpret_cast<HANDLE>(kPerMonitorAware))) {
                ::FreeLibrary(user32);
                return true;
            }
            if (::GetLastError() == ERROR_ACCESS_DENIED) {
                ::FreeLibrary(user32);
                return true;
            }
        }

        ::FreeLibrary(user32);
    }

    HMODULE shcore = ::LoadLibraryW(L"shcore.dll");
    if (shcore) {
        using SetProcessDpiAwarenessFn = HRESULT(WINAPI*)(int);
        auto set_awareness = reinterpret_cast<SetProcessDpiAwarenessFn>(
            ::GetProcAddress(shcore, "SetProcessDpiAwareness"));
        if (set_awareness) {
            constexpr int kProcessPerMonitorDpiAware = 2;
            HRESULT hr = set_awareness(kProcessPerMonitorDpiAware);
            if (SUCCEEDED(hr) || hr == E_ACCESSDENIED) {
                ::FreeLibrary(shcore);
                return true;
            }
        }
        ::FreeLibrary(shcore);
    }

    user32 = ::LoadLibraryW(L"user32.dll");
    if (user32) {
        using SetProcessDpiAwareFn = BOOL(WINAPI*)();
        auto set_system_aware = reinterpret_cast<SetProcessDpiAwareFn>(
            ::GetProcAddress(user32, "SetProcessDPIAware"));
        if (set_system_aware) {
            if (set_system_aware() || ::GetLastError() == ERROR_ACCESS_DENIED) {
                ::FreeLibrary(user32);
                return true;
            }
        }
        ::FreeLibrary(user32);
    }

    return false;
}

} // namespace acecode::desktop

#endif // _WIN32
