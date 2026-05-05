// Windows 系统托盘图标实现。
//
// 设计 / 决策见 openspec/changes/add-desktop-attention-notifications/design.md
// 决策 2(隐藏 message-only window 接 WM_USER 系列消息)。

#include "tray_icon_win.hpp"

#include "notifications_win.hpp"
#include "../utils/logger.hpp"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <shellapi.h>
#endif

namespace acecode::desktop {

#ifdef _WIN32

namespace {

constexpr wchar_t kTrayWndClass[] = L"ACECodeDesktopTrayWindow";
constexpr UINT kTrayIconUid = 0xACEC; // 任意常量,保持稳定即可
constexpr UINT kMenuIdShow = 1;
constexpr UINT kMenuIdQuit = 2;

UINT g_tray_callback_msg = 0; // 由 RegisterWindowMessageW 注册,避免与其他 WM_USER 撞号
HWND g_tray_window = nullptr;
TrayClickHandler g_on_show;
TrayClickHandler g_on_quit;
NOTIFYICONDATAW g_nid{};
bool g_icon_added = false;

HICON load_app_icon() {
    // acecode.rc.in 里把 acecode.ico 作为 IDI_ICON1 (id=1) 编入二进制。
    // 失败 → 用系统默认应用图标兜底,避免崩。
    HINSTANCE hinst = ::GetModuleHandleW(nullptr);
    if (HICON h = ::LoadIconW(hinst, MAKEINTRESOURCEW(1))) return h;
    // 显式用 W 版本宏,避免在 UNICODE 未定义的项目里走 MAKEINTRESOURCEA。
    // 32512 = IDI_APPLICATION 数值常量。
    return ::LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
}

void show_context_menu(HWND hwnd) {
    HMENU menu = ::CreatePopupMenu();
    if (!menu) return;
    ::AppendMenuW(menu, MF_STRING, kMenuIdShow, L"显示窗口");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kMenuIdQuit, L"退出");

    POINT pt{};
    ::GetCursorPos(&pt);
    // SetForegroundWindow 是 Win32 文档规定的 trick,让 popup menu 在 click-away 时
    // 正确关闭。不调的话菜单显示后单击它处不会消失。
    ::SetForegroundWindow(hwnd);
    UINT cmd = ::TrackPopupMenu(menu,
                                TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                                pt.x, pt.y, 0, hwnd, nullptr);
    ::DestroyMenu(menu);
    if (cmd == kMenuIdShow) {
        if (g_on_show) g_on_show();
    } else if (cmd == kMenuIdQuit) {
        if (g_on_quit) g_on_quit();
    }
}

LRESULT CALLBACK tray_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == g_tray_callback_msg && g_tray_callback_msg != 0) {
        // tray icon 事件。lparam 低 WORD 是事件类型 (WM_LBUTTONUP / WM_RBUTTONUP /
        // NIN_BALLOONUSERCLICK 等)。具体见 Shell_NotifyIcon 文档。
        UINT event = LOWORD(lparam);
        switch (event) {
            case WM_LBUTTONUP:
                if (g_on_show) g_on_show();
                return 0;
            case WM_CONTEXTMENU:
            case WM_RBUTTONUP:
                show_context_menu(hwnd);
                return 0;
            case NIN_BALLOONUSERCLICK:
                // 用户点了气泡(非图标本身)→ 路由到 notifications_win 派发 click_handler
                on_balloon_clicked();
                if (g_on_show) g_on_show();
                return 0;
            default:
                break;
        }
    }
    if (msg == WM_DESTROY) {
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool register_tray_class(HINSTANCE hinst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.hInstance = hinst;
    wc.lpszClassName = kTrayWndClass;
    wc.lpfnWndProc = tray_wnd_proc;
    if (::RegisterClassExW(&wc)) return true;
    DWORD err = ::GetLastError();
    return err == ERROR_CLASS_ALREADY_EXISTS;
}

} // namespace

// 让 notifications_win.cpp 拿到 tray icon 的 UID,piggyback 气泡时用同一 UID。
// 通过 extern 声明而非头文件公开,避免外部代码误用这个常量。
UINT tray_icon_uid_value() { return kTrayIconUid; }

bool init_tray_icon(TrayClickHandler on_show,
                    TrayClickHandler on_quit,
                    void** out_message_hwnd) {
    if (out_message_hwnd) *out_message_hwnd = nullptr;
    if (g_icon_added) {
        LOG_WARN("[desktop] init_tray_icon called twice, ignoring second call");
        if (out_message_hwnd) *out_message_hwnd = g_tray_window;
        return true;
    }

    HINSTANCE hinst = ::GetModuleHandleW(nullptr);
    if (!register_tray_class(hinst)) {
        LOG_WARN("[desktop] tray: RegisterClassExW failed, no tray icon");
        return false;
    }

    g_tray_window = ::CreateWindowExW(
        0,
        kTrayWndClass,
        L"ACECode Tray",
        0,
        0, 0, 0, 0,
        HWND_MESSAGE,  // message-only 窗口,不进 z-order
        nullptr,
        hinst,
        nullptr);
    if (!g_tray_window) {
        LOG_WARN("[desktop] tray: CreateWindowExW failed, no tray icon");
        return false;
    }

    g_tray_callback_msg = ::RegisterWindowMessageW(L"ACECode_TrayMessage_v1");
    g_on_show = std::move(on_show);
    g_on_quit = std::move(on_quit);

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_tray_window;
    g_nid.uID = kTrayIconUid;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = g_tray_callback_msg;
    g_nid.hIcon = load_app_icon();
    wcsncpy_s(g_nid.szTip, L"ACECode", _TRUNCATE);

    if (!::Shell_NotifyIconW(NIM_ADD, &g_nid)) {
        LOG_WARN("[desktop] tray: Shell_NotifyIconW NIM_ADD failed");
        ::DestroyWindow(g_tray_window);
        g_tray_window = nullptr;
        return false;
    }
    g_icon_added = true;
    if (out_message_hwnd) *out_message_hwnd = g_tray_window;
    LOG_INFO("[desktop] tray: icon installed");
    return true;
}

void shutdown_tray_icon() {
    if (g_icon_added) {
        ::Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_icon_added = false;
    }
    if (g_tray_window) {
        ::DestroyWindow(g_tray_window);
        g_tray_window = nullptr;
    }
    HINSTANCE hinst = ::GetModuleHandleW(nullptr);
    ::UnregisterClassW(kTrayWndClass, hinst);
    g_on_show = nullptr;
    g_on_quit = nullptr;
}

#else // !_WIN32

bool init_tray_icon(TrayClickHandler /*on_show*/,
                    TrayClickHandler /*on_quit*/,
                    void** out_message_hwnd) {
    if (out_message_hwnd) *out_message_hwnd = nullptr;
    return false;
}
void shutdown_tray_icon() {}

#endif // _WIN32

} // namespace acecode::desktop
