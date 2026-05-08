// Windows 系统托盘图标实现。
//
// 设计 / 决策见:
//   - openspec/changes/add-desktop-attention-notifications/design.md 决策 2
//     (隐藏 message-only window 接 WM_USER 系列消息)
//   - openspec/changes/enhance-desktop-tray-menu/design.md(小图标 / 双击 / Codex 菜单)

#include "tray_icon_win.hpp"

#include "notifications_win.hpp"
#include "tray_menu_layout.hpp"
#include "../utils/encoding.hpp"
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

#include <mutex>
#include <utility>

namespace acecode::desktop {

#ifdef _WIN32

namespace {

constexpr wchar_t kTrayWndClass[] = L"ACECodeDesktopTrayWindow";
constexpr UINT kTrayIconUid = 0xACEC; // 任意常量,保持稳定即可

UINT g_tray_callback_msg = 0; // 由 RegisterWindowMessageW 注册,避免与其他 WM_USER 撞号
HWND g_tray_window = nullptr;
TrayClickHandler g_on_show;
TrayClickHandler g_on_quit;
NOTIFYICONDATAW g_nid{};
bool g_icon_added = false;

// 菜单 payload + handlers,主线程读 + 多线程写,统一上锁。
std::mutex g_payload_mu;
TrayMenuPayload g_payload;
TraySessionClickHandler g_session_click_handler;
TrayClickHandler g_new_chat_handler;
TrayClickHandler g_open_app_handler;

HICON load_app_icon_for_tray() {
    // acecode.rc.in 里把 acecode.ico 作为 IDI_ICON1 (id=1) 编入二进制。
    // 加载策略(逐级 fallback,保证最差也是程序图标而不是系统通用图标):
    //   1. LoadImageW + SM_CXSMICON × SM_CYSMICON  → 16×16 frame,质感最佳
    //   2. LoadImageW + SM_CXICON × SM_CYICON      → 32×32 frame,shell downscale
    //   3. LoadIconW (MAKEINTRESOURCEW(1))         → 老路径,等价 LoadImage 32×32
    //   4. LoadIconW (IDI_APPLICATION)             → 终极兜底,系统默认图标
    HINSTANCE hinst = ::GetModuleHandleW(nullptr);
    int sx = ::GetSystemMetrics(SM_CXSMICON);
    int sy = ::GetSystemMetrics(SM_CYSMICON);
    if (HICON ic = static_cast<HICON>(::LoadImageW(
            hinst, MAKEINTRESOURCEW(1), IMAGE_ICON, sx, sy, LR_DEFAULTCOLOR))) {
        LOG_INFO("[desktop] tray icon: loaded small frame via LoadImageW " +
                 std::to_string(sx) + "x" + std::to_string(sy));
        return ic;
    }
    int lx = ::GetSystemMetrics(SM_CXICON);
    int ly = ::GetSystemMetrics(SM_CYICON);
    if (HICON ic = static_cast<HICON>(::LoadImageW(
            hinst, MAKEINTRESOURCEW(1), IMAGE_ICON, lx, ly, LR_DEFAULTCOLOR))) {
        LOG_WARN("[desktop] tray icon: small frame load failed, fell back to "
                 "large frame (will be downscaled by shell)");
        return ic;
    }
    if (HICON ic = ::LoadIconW(hinst, MAKEINTRESOURCEW(1))) {
        LOG_WARN("[desktop] tray icon: LoadImageW failed, fell back to LoadIconW");
        return ic;
    }
    LOG_ERROR("[desktop] tray icon: app icon resource missing, using IDI_APPLICATION");
    // 32512 = IDI_APPLICATION 数值常量。
    return ::LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
}

// 菜单 popup 用的快照拷贝。读 payload 后持锁释放,渲染期间不再持锁。
TrayMenuPayload snapshot_payload() {
    std::lock_guard<std::mutex> lk(g_payload_mu);
    return g_payload;
}

TraySessionClickHandler get_session_click_handler() {
    std::lock_guard<std::mutex> lk(g_payload_mu);
    return g_session_click_handler;
}

TrayClickHandler get_new_chat_handler() {
    std::lock_guard<std::mutex> lk(g_payload_mu);
    return g_new_chat_handler;
}

TrayClickHandler get_open_app_handler() {
    std::lock_guard<std::mutex> lk(g_payload_mu);
    return g_open_app_handler;
}

// 把 layout 翻译成 AppendMenuW 的实际调用。MoreSubmenuItem 收进 popup,
// 其余在顶层 menu 上 append。
void append_layout_to_menu(HMENU menu, const TrayMenuLayout& layout) {
    HMENU more_submenu = nullptr;
    bool more_root_inserted = false;
    for (const auto& e : layout.entries) {
        switch (e.kind) {
            case TrayMenuEntryKind::PinnedHeader:
            case TrayMenuEntryKind::RecentHeader: {
                std::wstring w = utf8_to_wide(e.label);
                ::AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, w.c_str());
                break;
            }
            case TrayMenuEntryKind::PinnedItem:
            case TrayMenuEntryKind::RecentItem:
            case TrayMenuEntryKind::NewChat:
            case TrayMenuEntryKind::OpenApp:
            case TrayMenuEntryKind::Quit: {
                std::wstring w = utf8_to_wide(e.label);
                ::AppendMenuW(menu, MF_STRING, e.id, w.c_str());
                break;
            }
            case TrayMenuEntryKind::Separator:
                ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                break;
            case TrayMenuEntryKind::MoreSubmenuRoot: {
                if (!more_submenu) more_submenu = ::CreatePopupMenu();
                std::wstring w = utf8_to_wide(e.label);
                ::AppendMenuW(menu,
                              MF_POPUP | MF_STRING,
                              reinterpret_cast<UINT_PTR>(more_submenu),
                              w.c_str());
                more_root_inserted = true;
                break;
            }
            case TrayMenuEntryKind::MoreSubmenuItem: {
                if (!more_submenu) more_submenu = ::CreatePopupMenu();
                std::wstring w = utf8_to_wide(e.label);
                ::AppendMenuW(more_submenu, MF_STRING, e.id, w.c_str());
                break;
            }
        }
    }
    (void)more_root_inserted; // 避免 unused 警告;子菜单的句柄交给 menu 拥有,DestroyMenu 时连带回收
}

// 由 cmd ID 反查 (workspace_hash, session_id),从 layout 直接线性查表。
// 命中后调 session click handler;未命中(ID 是 fixed item 但调用方没传 layout)返回 false。
bool dispatch_session_click(const TrayMenuLayout& layout, UINT cmd) {
    for (const auto& e : layout.entries) {
        if (e.id == cmd && (e.kind == TrayMenuEntryKind::PinnedItem ||
                             e.kind == TrayMenuEntryKind::RecentItem ||
                             e.kind == TrayMenuEntryKind::MoreSubmenuItem)) {
            auto handler = get_session_click_handler();
            if (handler) handler(e.workspace_hash, e.session_id);
            return true;
        }
    }
    return false;
}

void show_context_menu(HWND hwnd) {
    TrayMenuPayload payload = snapshot_payload();
    TrayMenuLayout layout = compute_menu_layout(payload);

    HMENU menu = ::CreatePopupMenu();
    if (!menu) return;
    append_layout_to_menu(menu, layout);

    POINT pt{};
    ::GetCursorPos(&pt);
    // SetForegroundWindow 是 Win32 文档规定的 trick,让 popup menu 在 click-away 时
    // 正确关闭。不调的话菜单显示后单击它处不会消失。
    ::SetForegroundWindow(hwnd);
    UINT cmd = ::TrackPopupMenu(menu,
                                TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                                pt.x, pt.y, 0, hwnd, nullptr);
    ::DestroyMenu(menu);

    if (cmd == 0) return;
    if (cmd == kMenuIdShow || cmd == kMenuIdOpenApp) {
        auto h = get_open_app_handler();
        if (h) h();
        else if (g_on_show) g_on_show();
    } else if (cmd == kMenuIdQuit) {
        if (g_on_quit) g_on_quit();
    } else if (cmd == kMenuIdNewChat) {
        auto h = get_new_chat_handler();
        if (h) h();
    } else {
        dispatch_session_click(layout, cmd);
    }
}

LRESULT CALLBACK tray_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == g_tray_callback_msg && g_tray_callback_msg != 0) {
        // tray icon 事件。lparam 低 WORD 是事件类型 (WM_LBUTTONUP / WM_RBUTTONUP /
        // NIN_BALLOONUSERCLICK 等)。具体见 Shell_NotifyIcon 文档。
        UINT event = LOWORD(lparam);
        switch (event) {
            case WM_LBUTTONUP:
            case WM_LBUTTONDBLCLK:
                // 双击与单击都路由到 on_show — bring_window_foreground 是幂等的,
                // 双击连发两次 SetForegroundWindow 没有观感差异。
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
    g_nid.hIcon = load_app_icon_for_tray();
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
    {
        std::lock_guard<std::mutex> lk(g_payload_mu);
        g_payload = TrayMenuPayload{};
        g_session_click_handler = nullptr;
        g_new_chat_handler = nullptr;
        g_open_app_handler = nullptr;
    }
}

void set_tray_menu_payload(TrayMenuPayload payload) {
    std::lock_guard<std::mutex> lk(g_payload_mu);
    g_payload = std::move(payload);
}

void clear_tray_menu_payload() {
    std::lock_guard<std::mutex> lk(g_payload_mu);
    g_payload = TrayMenuPayload{};
}

void set_tray_session_click_handler(TraySessionClickHandler handler) {
    std::lock_guard<std::mutex> lk(g_payload_mu);
    g_session_click_handler = std::move(handler);
}

void set_tray_new_chat_handler(TrayClickHandler handler) {
    std::lock_guard<std::mutex> lk(g_payload_mu);
    g_new_chat_handler = std::move(handler);
}

void set_tray_open_app_handler(TrayClickHandler handler) {
    std::lock_guard<std::mutex> lk(g_payload_mu);
    g_open_app_handler = std::move(handler);
}

#else // !_WIN32

bool init_tray_icon(TrayClickHandler /*on_show*/,
                    TrayClickHandler /*on_quit*/,
                    void** out_message_hwnd) {
    if (out_message_hwnd) *out_message_hwnd = nullptr;
    return false;
}
void shutdown_tray_icon() {}

void set_tray_menu_payload(TrayMenuPayload /*payload*/) {}
void clear_tray_menu_payload() {}
void set_tray_session_click_handler(TraySessionClickHandler /*handler*/) {}
void set_tray_new_chat_handler(TrayClickHandler /*handler*/) {}
void set_tray_open_app_handler(TrayClickHandler /*handler*/) {}

#endif // _WIN32

} // namespace acecode::desktop
