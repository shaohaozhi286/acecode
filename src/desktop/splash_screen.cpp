#include "splash_screen.hpp"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>

#  include <algorithm>
#  include <cstring>
#  include <filesystem>
#  include <string>
#endif

namespace acecode::desktop {

#ifdef _WIN32

namespace {

constexpr wchar_t kSplashClassName[] = L"ACECodeSplashWindow";

HMONITOR active_monitor() {
    if (HWND fg = ::GetForegroundWindow()) {
        if (HMONITOR m = ::MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST)) {
            return m;
        }
    }
    POINT pt{};
    if (::GetCursorPos(&pt)) {
        if (HMONITOR m = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST)) {
            return m;
        }
    }
    return ::MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY);
}

RECT active_monitor_rect() {
    RECT fallback{
        0,
        0,
        ::GetSystemMetrics(SM_CXSCREEN),
        ::GetSystemMetrics(SM_CYSCREEN),
    };
    HMONITOR monitor = active_monitor();
    if (!monitor) return fallback;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!::GetMonitorInfoW(monitor, &mi)) return fallback;
    return mi.rcMonitor;
}

int choose_icon_size(const RECT& rc) {
    const int w = std::max(1, static_cast<int>(rc.right - rc.left));
    const int h = std::max(1, static_cast<int>(rc.bottom - rc.top));
    const int bounded = std::min({256, std::max(160, std::min(w, h) / 6)});
    return bounded;
}

std::wstring bundled_icon_path() {
    wchar_t buf[MAX_PATH] = {0};
    DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return {};
    std::filesystem::path cur = std::filesystem::path(std::wstring(buf, n)).parent_path();
    for (int i = 0; i < 7; ++i) {
        auto candidate = cur / "assets" / "windows" / "acecode.ico";
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec) && !ec) {
            return candidate.wstring();
        }
        if (!cur.has_parent_path()) break;
        cur = cur.parent_path();
    }
    return {};
}

} // namespace

struct SplashScreen::Impl {
    HWND hwnd = nullptr;
    HICON icon = nullptr;
    bool destroy_icon = false;
    POINT pos{};
    int icon_size = 192;

    ~Impl() {
        close();
    }

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        }
        auto* self = reinterpret_cast<Impl*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (msg) {
            case WM_ERASEBKGND:
                return 1;
            default:
                break;
        }
        return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    static bool register_class(HINSTANCE instance) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = Impl::wnd_proc;
        wc.hInstance = instance;
        wc.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
        wc.lpszClassName = kSplashClassName;
        if (::RegisterClassW(&wc)) return true;
        return ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    void show() {
        if (hwnd) return;

        HINSTANCE instance = ::GetModuleHandleW(nullptr);
        if (!register_class(instance)) return;

        RECT rect = active_monitor_rect();
        icon_size = choose_icon_size(rect);
        load_icon(instance);
        if (!icon) return;

        const int monitor_w = std::max(1, static_cast<int>(rect.right - rect.left));
        const int monitor_h = std::max(1, static_cast<int>(rect.bottom - rect.top));
        pos.x = rect.left + (monitor_w - icon_size) / 2;
        pos.y = rect.top + (monitor_h - icon_size) / 2;
        hwnd = ::CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kSplashClassName,
            L"ACECode",
            WS_POPUP,
            pos.x,
            pos.y,
            icon_size,
            icon_size,
            nullptr,
            nullptr,
            instance,
            this);
        if (!hwnd) return;

        render_layered_icon();
        ::SetWindowPos(hwnd, HWND_TOPMOST, pos.x, pos.y, icon_size, icon_size,
                       SWP_NOACTIVATE | SWP_SHOWWINDOW);
        ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    }

    void close() {
        if (hwnd) {
            ::DestroyWindow(hwnd);
            hwnd = nullptr;
        }
        if (icon && destroy_icon) {
            ::DestroyIcon(icon);
        }
        icon = nullptr;
        destroy_icon = false;
    }

    void load_icon(HINSTANCE instance) {
        icon = static_cast<HICON>(::LoadImageW(
            instance,
            L"IDI_ICON1",
            IMAGE_ICON,
            icon_size,
            icon_size,
            LR_DEFAULTCOLOR));
        destroy_icon = icon != nullptr;
        if (icon) return;

        auto path = bundled_icon_path();
        if (!path.empty()) {
            icon = static_cast<HICON>(::LoadImageW(
                nullptr,
                path.c_str(),
                IMAGE_ICON,
                icon_size,
                icon_size,
                LR_LOADFROMFILE | LR_DEFAULTCOLOR));
            destroy_icon = icon != nullptr;
            if (icon) return;
        }

        icon = ::LoadIconW(instance, L"IDI_ICON1");
        destroy_icon = false;
    }

    void render_layered_icon() {
        HDC screen = ::GetDC(nullptr);
        if (!screen) return;

        HDC mem = ::CreateCompatibleDC(screen);
        if (!mem) {
            ::ReleaseDC(nullptr, screen);
            return;
        }

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = icon_size;
        bmi.bmiHeader.biHeight = -icon_size;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP bitmap = ::CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bitmap) {
            ::DeleteDC(mem);
            ::ReleaseDC(nullptr, screen);
            return;
        }

        std::memset(bits, 0, static_cast<size_t>(icon_size) * static_cast<size_t>(icon_size) * 4);
        HBITMAP old = static_cast<HBITMAP>(::SelectObject(mem, bitmap));
        ::DrawIconEx(mem, 0, 0, icon, icon_size, icon_size, 0, nullptr, DI_NORMAL);

        POINT src{0, 0};
        SIZE size{icon_size, icon_size};
        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;
        ::UpdateLayeredWindow(hwnd, screen, &pos, &size, mem, &src, 0, &blend, ULW_ALPHA);

        ::SelectObject(mem, old);
        ::DeleteObject(bitmap);
        ::DeleteDC(mem);
        ::ReleaseDC(nullptr, screen);
    }
};

#else

struct SplashScreen::Impl {
    void show() {}
    void close() {}
};

#endif

SplashScreen::~SplashScreen() {
    close();
}

void SplashScreen::show() {
    if (!impl_) impl_ = new Impl();
    impl_->show();
}

void SplashScreen::close() {
    if (!impl_) return;
    impl_->close();
    delete impl_;
    impl_ = nullptr;
}

} // namespace acecode::desktop
