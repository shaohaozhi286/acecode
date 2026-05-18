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
#ifdef __APPLE__
#  import <AppKit/AppKit.h>
#endif

#include <mutex>
#include <cstdint>
#include <utility>
#include <vector>

#if !defined(_WIN32) && !defined(__APPLE__)
#  include <dlfcn.h>
#  include <filesystem>
#  include <limits.h>
#  include <unistd.h>
#endif

#ifdef __APPLE__
namespace acecode::desktop {
void mac_tray_menu_item_activated(long tag);
void mac_tray_status_item_activated();
void mac_tray_menu_needs_update(NSMenu* menu);
}

@interface ACECodeTrayMenuTarget : NSObject <NSMenuDelegate>
- (void)menuItemActivated:(id)sender;
- (void)statusItemActivated:(id)sender;
@end

@implementation ACECodeTrayMenuTarget
- (void)menuItemActivated:(id)sender {
    if (![sender respondsToSelector:@selector(tag)]) return;
    acecode::desktop::mac_tray_menu_item_activated([sender tag]);
}

- (void)statusItemActivated:(id)sender {
    (void)sender;
    acecode::desktop::mac_tray_status_item_activated();
}

- (void)menuNeedsUpdate:(NSMenu*)menu {
    acecode::desktop::mac_tray_menu_needs_update(menu);
}
@end
#endif

namespace acecode::desktop {

namespace {

// 菜单 payload + handlers,主线程读 + 多线程写,统一上锁。Win32 和 Linux
// 后端共享这份状态,避免复制 pinned/recent/new/open/quit 的分发规则。
std::mutex g_payload_mu;
TrayMenuPayload g_payload;
TraySessionClickHandler g_session_click_handler;
TrayClickHandler g_new_chat_handler;
TrayClickHandler g_open_app_handler;
TrayClickHandler g_on_show;
TrayClickHandler g_on_quit;

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

bool dispatch_session_click(const TrayMenuLayout& layout, unsigned cmd) {
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

void dispatch_menu_command(const TrayMenuLayout& layout, unsigned cmd) {
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

void reset_tray_state() {
    g_on_show = nullptr;
    g_on_quit = nullptr;
    std::lock_guard<std::mutex> lk(g_payload_mu);
    g_payload = TrayMenuPayload{};
    g_session_click_handler = nullptr;
    g_new_chat_handler = nullptr;
    g_open_app_handler = nullptr;
}

} // namespace

#ifdef _WIN32

namespace {

constexpr wchar_t kTrayWndClass[] = L"ACECodeDesktopTrayWindow";
constexpr UINT kTrayIconUid = 0xACEC; // 任意常量,保持稳定即可
constexpr wchar_t kAppIconResourceName[] = L"IDI_ICON1";
constexpr int kAppIconResourceId = 1;

UINT g_tray_callback_msg = 0; // 由 RegisterWindowMessageW 注册,避免与其他 WM_USER 撞号
HWND g_tray_window = nullptr;
NOTIFYICONDATAW g_nid{};
bool g_icon_added = false;

HICON load_app_icon_for_tray() {
    // acecode.rc.in 把 acecode.ico 编成 IDI_ICON1。旧构建里 IDI_ICON1 可能是
    // 命名资源,新构建里是数字资源(1),所以两种都试一遍。
    HINSTANCE hinst = ::GetModuleHandleW(nullptr);
    int sx = ::GetSystemMetrics(SM_CXSMICON);
    int sy = ::GetSystemMetrics(SM_CYSMICON);
    if (HICON ic = static_cast<HICON>(::LoadImageW(
            hinst, kAppIconResourceName, IMAGE_ICON, sx, sy, LR_DEFAULTCOLOR))) {
        LOG_INFO("[desktop] tray icon: loaded named small frame via LoadImageW " +
                 std::to_string(sx) + "x" + std::to_string(sy));
        return ic;
    }
    if (HICON ic = static_cast<HICON>(::LoadImageW(
            hinst, MAKEINTRESOURCEW(kAppIconResourceId), IMAGE_ICON, sx, sy, LR_DEFAULTCOLOR))) {
        LOG_INFO("[desktop] tray icon: loaded small frame via LoadImageW " +
                 std::to_string(sx) + "x" + std::to_string(sy));
        return ic;
    }
    int lx = ::GetSystemMetrics(SM_CXICON);
    int ly = ::GetSystemMetrics(SM_CYICON);
    if (HICON ic = static_cast<HICON>(::LoadImageW(
            hinst, kAppIconResourceName, IMAGE_ICON, lx, ly, LR_DEFAULTCOLOR))) {
        LOG_WARN("[desktop] tray icon: small frame load failed, fell back to "
                 "named large frame (will be downscaled by shell)");
        return ic;
    }
    if (HICON ic = static_cast<HICON>(::LoadImageW(
            hinst, MAKEINTRESOURCEW(kAppIconResourceId), IMAGE_ICON, lx, ly, LR_DEFAULTCOLOR))) {
        LOG_WARN("[desktop] tray icon: small frame load failed, fell back to "
                 "large frame (will be downscaled by shell)");
        return ic;
    }
    if (HICON ic = ::LoadIconW(hinst, kAppIconResourceName)) {
        LOG_WARN("[desktop] tray icon: LoadImageW failed, fell back to named LoadIconW");
        return ic;
    }
    if (HICON ic = ::LoadIconW(hinst, MAKEINTRESOURCEW(kAppIconResourceId))) {
        LOG_WARN("[desktop] tray icon: LoadImageW failed, fell back to LoadIconW");
        return ic;
    }
    LOG_ERROR("[desktop] tray icon: app icon resource missing, using IDI_APPLICATION");
    // 32512 = IDI_APPLICATION 数值常量。
    return ::LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
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

    dispatch_menu_command(layout, cmd);
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
    reset_tray_state();
}

#elif !defined(__APPLE__)

namespace {

namespace fs = std::filesystem;

struct GtkTrayApi {
    using GtkStatusIconNewFromFile = void* (*)(const char*);
    using GtkStatusIconNewFromIconName = void* (*)(const char*);
    using GtkStatusIconSetTooltipText = void (*)(void*, const char*);
    using GtkStatusIconSetVisible = void (*)(void*, int);
    using GtkMenuNew = void* (*)();
    using GtkMenuItemNewWithLabel = void* (*)(const char*);
    using GtkMenuItemSetSubmenu = void (*)(void*, void*);
    using GtkSeparatorMenuItemNew = void* (*)();
    using GtkMenuShellAppend = void (*)(void*, void*);
    using GtkWidgetShowAll = void (*)(void*);
    using GtkMenuPopupAtPointer = void (*)(void*, const void*);
    using GtkWidgetDestroy = void (*)(void*);
    using GSignalConnectData = unsigned long (*)(void*, const char*, void*, void*, void*, int);
    using GObjectUnref = void (*)(void*);

    void* gtk = nullptr;
    void* gobject = nullptr;
    GtkStatusIconNewFromFile status_new_from_file = nullptr;
    GtkStatusIconNewFromIconName status_new_from_icon_name = nullptr;
    GtkStatusIconSetTooltipText status_set_tooltip_text = nullptr;
    GtkStatusIconSetVisible status_set_visible = nullptr;
    GtkMenuNew menu_new = nullptr;
    GtkMenuItemNewWithLabel menu_item_new_with_label = nullptr;
    GtkMenuItemSetSubmenu menu_item_set_submenu = nullptr;
    GtkSeparatorMenuItemNew separator_menu_item_new = nullptr;
    GtkMenuShellAppend menu_shell_append = nullptr;
    GtkWidgetShowAll widget_show_all = nullptr;
    GtkMenuPopupAtPointer menu_popup_at_pointer = nullptr;
    GtkWidgetDestroy widget_destroy = nullptr;
    GSignalConnectData signal_connect_data = nullptr;
    GObjectUnref object_unref = nullptr;

    bool load() {
        if (gtk && gobject) return true;
        gtk = ::dlopen("libgtk-3.so.0", RTLD_LAZY | RTLD_LOCAL);
        gobject = ::dlopen("libgobject-2.0.so.0", RTLD_LAZY | RTLD_LOCAL);
        if (!gtk || !gobject) {
            LOG_WARN("[desktop] tray: GTK3/GObject runtime not available");
            return false;
        }
        auto sym = [](void* lib, const char* name) -> void* {
            return ::dlsym(lib, name);
        };
        status_new_from_file = reinterpret_cast<GtkStatusIconNewFromFile>(
            sym(gtk, "gtk_status_icon_new_from_file"));
        status_new_from_icon_name = reinterpret_cast<GtkStatusIconNewFromIconName>(
            sym(gtk, "gtk_status_icon_new_from_icon_name"));
        status_set_tooltip_text = reinterpret_cast<GtkStatusIconSetTooltipText>(
            sym(gtk, "gtk_status_icon_set_tooltip_text"));
        status_set_visible = reinterpret_cast<GtkStatusIconSetVisible>(
            sym(gtk, "gtk_status_icon_set_visible"));
        menu_new = reinterpret_cast<GtkMenuNew>(sym(gtk, "gtk_menu_new"));
        menu_item_new_with_label = reinterpret_cast<GtkMenuItemNewWithLabel>(
            sym(gtk, "gtk_menu_item_new_with_label"));
        menu_item_set_submenu = reinterpret_cast<GtkMenuItemSetSubmenu>(
            sym(gtk, "gtk_menu_item_set_submenu"));
        separator_menu_item_new = reinterpret_cast<GtkSeparatorMenuItemNew>(
            sym(gtk, "gtk_separator_menu_item_new"));
        menu_shell_append = reinterpret_cast<GtkMenuShellAppend>(
            sym(gtk, "gtk_menu_shell_append"));
        widget_show_all = reinterpret_cast<GtkWidgetShowAll>(
            sym(gtk, "gtk_widget_show_all"));
        menu_popup_at_pointer = reinterpret_cast<GtkMenuPopupAtPointer>(
            sym(gtk, "gtk_menu_popup_at_pointer"));
        widget_destroy = reinterpret_cast<GtkWidgetDestroy>(
            sym(gtk, "gtk_widget_destroy"));
        signal_connect_data = reinterpret_cast<GSignalConnectData>(
            sym(gobject, "g_signal_connect_data"));
        object_unref = reinterpret_cast<GObjectUnref>(sym(gobject, "g_object_unref"));
        return status_set_tooltip_text && status_set_visible && menu_new &&
               menu_item_new_with_label && menu_item_set_submenu &&
               separator_menu_item_new && menu_shell_append &&
               widget_show_all && menu_popup_at_pointer && widget_destroy &&
               signal_connect_data && object_unref &&
               (status_new_from_file || status_new_from_icon_name);
    }
};

GtkTrayApi g_gtk;
void* g_status_icon = nullptr;
void* g_context_menu = nullptr;
TrayMenuLayout g_context_layout;
bool g_linux_tray_installed = false;

std::string linux_exe_dir() {
    std::vector<char> buf(static_cast<size_t>(PATH_MAX) + 1);
    ssize_t n = ::readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (n <= 0) return {};
    buf[static_cast<size_t>(n)] = '\0';
    std::error_code ec;
    fs::path resolved = fs::weakly_canonical(fs::path(buf.data()), ec);
    if (ec) resolved = fs::path(buf.data());
    return resolved.parent_path().string();
}

std::string find_linux_tray_icon() {
    std::vector<fs::path> roots;
    if (auto dir = linux_exe_dir(); !dir.empty()) roots.push_back(fs::path(dir));
    std::error_code ec;
    roots.push_back(fs::current_path(ec));
    for (auto root : roots) {
        for (int i = 0; i < 8 && !root.empty(); ++i) {
            for (const auto& rel : {
                     fs::path("acecode-logo.png"),
                     fs::path("web/dist/acecode-logo.png"),
                     fs::path("assets/windows/acecode_icon.png"),
                 }) {
                fs::path candidate = root / rel;
                std::error_code exists_ec;
                if (fs::exists(candidate, exists_ec) && !exists_ec) {
                    return candidate.string();
                }
            }
            if (!root.has_parent_path()) break;
            fs::path parent = root.parent_path();
            if (parent == root) break;
            root = std::move(parent);
        }
    }
    return {};
}

extern "C" void linux_tray_activate(void*, void*) {
    if (g_on_show) g_on_show();
}

extern "C" void linux_menu_item_activate(void*, void* data) {
    auto cmd = static_cast<unsigned>(reinterpret_cast<std::uintptr_t>(data));
    dispatch_menu_command(g_context_layout, cmd);
}

void append_linux_menu_item(void* menu, const TrayMenuEntry& entry) {
    void* item = nullptr;
    if (entry.kind == TrayMenuEntryKind::Separator) {
        item = g_gtk.separator_menu_item_new();
    } else {
        item = g_gtk.menu_item_new_with_label(entry.label.c_str());
        if (entry.id != 0 && entry.kind != TrayMenuEntryKind::PinnedHeader &&
            entry.kind != TrayMenuEntryKind::RecentHeader &&
            entry.kind != TrayMenuEntryKind::MoreSubmenuRoot) {
            auto data = reinterpret_cast<void*>(
                static_cast<std::uintptr_t>(entry.id));
            g_gtk.signal_connect_data(
                item, "activate", reinterpret_cast<void*>(linux_menu_item_activate),
                data, nullptr, 0);
        }
    }
    if (item) g_gtk.menu_shell_append(menu, item);
}

void append_linux_layout_to_menu(void* menu, const TrayMenuLayout& layout) {
    void* more_menu = nullptr;
    for (const auto& entry : layout.entries) {
        if (entry.kind == TrayMenuEntryKind::MoreSubmenuRoot) {
            void* root = g_gtk.menu_item_new_with_label(entry.label.c_str());
            more_menu = g_gtk.menu_new();
            if (root && more_menu) {
                g_gtk.menu_item_set_submenu(root, more_menu);
                g_gtk.menu_shell_append(menu, root);
            }
        } else if (entry.kind == TrayMenuEntryKind::MoreSubmenuItem) {
            append_linux_menu_item(more_menu ? more_menu : menu, entry);
        } else {
            append_linux_menu_item(menu, entry);
        }
    }
}

void show_linux_context_menu() {
    if (!g_linux_tray_installed) return;
    if (g_context_menu) {
        g_gtk.widget_destroy(g_context_menu);
        g_context_menu = nullptr;
    }
    TrayMenuPayload payload = snapshot_payload();
    g_context_layout = compute_menu_layout(payload);
    g_context_menu = g_gtk.menu_new();
    if (!g_context_menu) return;
    append_linux_layout_to_menu(g_context_menu, g_context_layout);
    g_gtk.widget_show_all(g_context_menu);
    g_gtk.menu_popup_at_pointer(g_context_menu, nullptr);
}

extern "C" void linux_tray_popup_menu(void*, unsigned, unsigned, void*) {
    show_linux_context_menu();
}

} // namespace

bool init_tray_icon(TrayClickHandler on_show,
                    TrayClickHandler on_quit,
                    void** out_message_hwnd) {
    if (out_message_hwnd) *out_message_hwnd = nullptr;
    if (g_linux_tray_installed) {
        LOG_WARN("[desktop] init_tray_icon called twice, ignoring second call");
        return true;
    }
    if (!g_gtk.load()) return false;

    std::string icon_path = find_linux_tray_icon();
    if (!icon_path.empty() && g_gtk.status_new_from_file) {
        g_status_icon = g_gtk.status_new_from_file(icon_path.c_str());
        LOG_INFO("[desktop] tray: loading Linux tray icon from " + icon_path);
    }
    if (!g_status_icon && g_gtk.status_new_from_icon_name) {
        g_status_icon = g_gtk.status_new_from_icon_name("utilities-terminal");
        LOG_WARN("[desktop] tray: ACECode icon not found, using fallback icon name");
    }
    if (!g_status_icon) return false;

    g_on_show = std::move(on_show);
    g_on_quit = std::move(on_quit);
    g_gtk.status_set_tooltip_text(g_status_icon, "ACECode");
    g_gtk.signal_connect_data(
        g_status_icon, "activate", reinterpret_cast<void*>(linux_tray_activate),
        nullptr, nullptr, 0);
    g_gtk.signal_connect_data(
        g_status_icon, "popup-menu", reinterpret_cast<void*>(linux_tray_popup_menu),
        nullptr, nullptr, 0);
    g_gtk.status_set_visible(g_status_icon, 1);
    g_linux_tray_installed = true;
    LOG_INFO("[desktop] tray: Linux GTK status icon installed");
    return true;
}

void shutdown_tray_icon() {
    if (g_context_menu) {
        g_gtk.widget_destroy(g_context_menu);
        g_context_menu = nullptr;
    }
    if (g_status_icon) {
        g_gtk.status_set_visible(g_status_icon, 0);
        g_gtk.object_unref(g_status_icon);
        g_status_icon = nullptr;
    }
    g_context_layout = TrayMenuLayout{};
    g_linux_tray_installed = false;
    reset_tray_state();
}

#elif defined(__APPLE__)

namespace {

NSStatusItem* g_status_item = nil;
NSMenu* g_status_menu = nil;
TrayMenuLayout g_mac_context_layout;

ACECodeTrayMenuTarget* g_menu_target = nil;

void append_mac_menu_item(NSMenu* menu, const TrayMenuEntry& entry) {
    if (!menu) return;
    if (entry.kind == TrayMenuEntryKind::Separator) {
        [menu addItem:[NSMenuItem separatorItem]];
        return;
    }

    NSString* label = [NSString stringWithUTF8String:entry.label.c_str()];
    if (!label) label = @"";
    NSMenuItem* item = [[NSMenuItem alloc]
        initWithTitle:label
               action:nil
        keyEquivalent:@""];
    [item setTarget:g_menu_target];
    [item setTag:static_cast<NSInteger>(entry.id)];

    if (entry.kind == TrayMenuEntryKind::PinnedHeader ||
        entry.kind == TrayMenuEntryKind::RecentHeader) {
        [item setEnabled:NO];
    } else if (entry.id != 0 && entry.kind != TrayMenuEntryKind::MoreSubmenuRoot) {
        [item setAction:@selector(menuItemActivated:)];
    }

    [menu addItem:item];
    [item release];
}

void append_mac_layout_to_menu(NSMenu* menu, const TrayMenuLayout& layout) {
    NSMenu* more_menu = nil;
    for (const auto& entry : layout.entries) {
        if (entry.kind == TrayMenuEntryKind::MoreSubmenuRoot) {
            NSString* label = [NSString stringWithUTF8String:entry.label.c_str()];
            if (!label) label = @"";
            NSMenuItem* root = [[NSMenuItem alloc]
                initWithTitle:label
                       action:nil
                keyEquivalent:@""];
            more_menu = [[NSMenu alloc] initWithTitle:label];
            [root setSubmenu:more_menu];
            [menu addItem:root];
            [more_menu release];
            [root release];
        } else if (entry.kind == TrayMenuEntryKind::MoreSubmenuItem) {
            append_mac_menu_item(more_menu ? more_menu : menu, entry);
        } else {
            append_mac_menu_item(menu, entry);
        }
    }
}

NSImage* load_status_item_image() {
    NSBundle* bundle = [NSBundle mainBundle];
    NSString* icon_path = [bundle pathForResource:@"acecode" ofType:@"icns"];
    NSImage* image = nil;
    if (icon_path) {
        image = [[[NSImage alloc] initWithContentsOfFile:icon_path] autorelease];
    }
    if (!image) {
        image = [NSApp applicationIconImage];
    }
    if (image) {
        [image setSize:NSMakeSize(18.0, 18.0)];
    }
    return image;
}

} // namespace

void mac_tray_menu_item_activated(long tag) {
    auto cmd = static_cast<unsigned>(tag);
    dispatch_menu_command(g_mac_context_layout, cmd);
}

void mac_tray_status_item_activated() {
    if (g_on_show) g_on_show();
}

void mac_tray_menu_needs_update(NSMenu* menu) {
    if (!menu) return;
    [menu removeAllItems];
    TrayMenuPayload payload = snapshot_payload();
    g_mac_context_layout = compute_menu_layout(payload);
    append_mac_layout_to_menu(menu, g_mac_context_layout);
}

bool init_tray_icon(TrayClickHandler on_show,
                    TrayClickHandler on_quit,
                    void** out_message_hwnd) {
    if (out_message_hwnd) *out_message_hwnd = nullptr;
    if (g_status_item) {
        LOG_WARN("[desktop] init_tray_icon called twice, ignoring second call");
        return true;
    }

    g_on_show = std::move(on_show);
    g_on_quit = std::move(on_quit);
    g_menu_target = [[ACECodeTrayMenuTarget alloc] init];
    g_status_menu = [[NSMenu alloc] initWithTitle:@"ACECode"];
    [g_status_menu setDelegate:g_menu_target];
    [g_status_menu setAutoenablesItems:NO];

    g_status_item = [[[NSStatusBar systemStatusBar]
        statusItemWithLength:NSSquareStatusItemLength] retain];
    if (!g_status_item) {
        [g_status_menu release];
        g_status_menu = nil;
        [g_menu_target release];
        g_menu_target = nil;
        reset_tray_state();
        return false;
    }

    NSStatusBarButton* button = [g_status_item button];
    if (button) {
        NSImage* image = load_status_item_image();
        if (image) {
            [button setImage:image];
        } else {
            [button setTitle:@"ACE"];
        }
        [button setToolTip:@"ACECode"];
        [button setTarget:g_menu_target];
        [button setAction:@selector(statusItemActivated:)];
        [button sendActionOn:NSEventMaskLeftMouseUp | NSEventMaskRightMouseUp];
    }
    [g_status_item setMenu:g_status_menu];

    LOG_INFO("[desktop] tray: macOS status item installed");
    return true;
}

void shutdown_tray_icon() {
    if (g_status_item) {
        [[NSStatusBar systemStatusBar] removeStatusItem:g_status_item];
        [g_status_item release];
        g_status_item = nil;
    }
    if (g_status_menu) {
        [g_status_menu setDelegate:nil];
        [g_status_menu release];
        g_status_menu = nil;
    }
    if (g_menu_target) {
        [g_menu_target release];
        g_menu_target = nil;
    }
    g_mac_context_layout = TrayMenuLayout{};
    reset_tray_state();
}

#else // other non-Linux platforms

bool init_tray_icon(TrayClickHandler /*on_show*/,
                    TrayClickHandler /*on_quit*/,
                    void** out_message_hwnd) {
    if (out_message_hwnd) *out_message_hwnd = nullptr;
    return false;
}
void shutdown_tray_icon() {
    reset_tray_state();
}

#endif // _WIN32

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

} // namespace acecode::desktop
