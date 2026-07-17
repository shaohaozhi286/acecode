// Windows 系统托盘图标实现。
//
// 设计 / 决策见:
//   - openspec/changes/enhance-desktop-tray-menu/design.md(小图标 / 双击 / Codex 菜单)

#include "tray_icon_win.hpp"

#include "tray_menu_layout.hpp"
#include "tray_menu_popup_model.hpp"
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

#include <algorithm>
#include <memory>
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

constexpr wchar_t kTrayPopupWndClass[] = L"ACECodeDesktopTrayPopupWindow";
constexpr UINT kTrayPopupDismissMessage = WM_APP + 0x351;

struct Win32TrayPopupMetrics {
    int dpi = 96;
    int width = 316;
    int outer_padding = 8;
    int horizontal_padding = 20;
    int text_gap = 16;
    int header_height = 34;
    int item_height = 36;
    int action_height = 44;
    int separator_height = 17;
    int corner_radius = 12;
    int right_column_width = 0;
};

struct Win32TrayPopupRow {
    TrayPopupRow model;
    int top = 0;
    int height = 0;
};

struct Win32TrayPopupController;

struct Win32TrayPopupWindow {
    Win32TrayPopupController* controller = nullptr;
    HWND hwnd = nullptr;
    bool is_submenu = false;
    Win32TrayPopupMetrics metrics;
    std::vector<Win32TrayPopupRow> rows;
    int content_height = 0;
    int viewport_height = 0;
    int scroll_offset = 0;
    int hovered_index = -1;
    HFONT font = nullptr;

    ~Win32TrayPopupWindow() {
        if (font) ::DeleteObject(font);
    }
};

struct Win32TrayPopupController {
    TrayMenuLayout layout;
    std::unique_ptr<Win32TrayPopupWindow> main_window;
    std::unique_ptr<Win32TrayPopupWindow> submenu_window;
    int main_more_index = -1;
    bool keyboard_in_submenu = false;
    bool closing = false;
};

std::unique_ptr<Win32TrayPopupController> g_tray_popup;

LRESULT CALLBACK tray_popup_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

int scale_px(int value, int dpi) {
    return ::MulDiv(value, dpi > 0 ? dpi : 96, 96);
}

int hdc_dpi(HDC hdc) {
    const int dpi = hdc ? ::GetDeviceCaps(hdc, LOGPIXELSX) : 96;
    return dpi > 0 ? dpi : 96;
}

int point_dpi(POINT pt) {
    HMONITOR monitor = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (HMODULE shcore = ::LoadLibraryW(L"Shcore.dll")) {
        using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
        auto get_dpi = reinterpret_cast<GetDpiForMonitorFn>(
            ::GetProcAddress(shcore, "GetDpiForMonitor"));
        UINT x = 96;
        UINT y = 96;
        if (get_dpi && SUCCEEDED(get_dpi(monitor, 0, &x, &y)) && x > 0) {
            ::FreeLibrary(shcore);
            return static_cast<int>(x);
        }
        ::FreeLibrary(shcore);
    }
    HDC hdc = ::GetDC(nullptr);
    const int dpi = hdc_dpi(hdc);
    if (hdc) ::ReleaseDC(nullptr, hdc);
    return dpi;
}

HFONT create_popup_font(int dpi) {
    return ::CreateFontW(
        -scale_px(14, dpi),
        0,
        0,
        0,
        FW_NORMAL,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");
}

int measure_text_width(HDC hdc, const std::wstring& text) {
    if (!hdc || text.empty()) return 0;
    SIZE size{};
    if (::GetTextExtentPoint32W(hdc, text.c_str(), static_cast<int>(text.size()), &size)) {
        return size.cx;
    }
    RECT rc{0, 0, 0, 0};
    ::DrawTextW(hdc,
                text.c_str(),
                static_cast<int>(text.size()),
                &rc,
                DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
    return rc.right - rc.left;
}

Win32TrayPopupMetrics compute_popup_metrics(const TrayMenuLayout& layout, int dpi) {
    HDC hdc = ::GetDC(nullptr);
    HFONT font = create_popup_font(dpi);
    HGDIOBJ old_font = nullptr;
    if (hdc && font) old_font = ::SelectObject(hdc, font);

    int max_subtitle_width = 0;
    for (const auto& e : layout.entries) {
        if (!tray_popup_entry_is_session(e.kind)) continue;
        max_subtitle_width = std::max(max_subtitle_width, measure_text_width(hdc, utf8_to_wide(e.subtitle)));
    }

    if (old_font) ::SelectObject(hdc, old_font);
    if (font) ::DeleteObject(font);
    if (hdc) ::ReleaseDC(nullptr, hdc);

    Win32TrayPopupMetrics metrics;
    metrics.dpi = dpi;
    metrics.outer_padding = scale_px(8, dpi);
    metrics.horizontal_padding = scale_px(20, dpi);
    metrics.text_gap = scale_px(16, dpi);
    metrics.header_height = scale_px(34, dpi);
    metrics.item_height = scale_px(28, dpi);
    metrics.action_height = scale_px(44, dpi);
    metrics.separator_height = std::max(1, scale_px(1, dpi));
    metrics.corner_radius = scale_px(12, dpi);
    if (max_subtitle_width > 0) {
        metrics.right_column_width = std::clamp(
            max_subtitle_width,
            scale_px(64, dpi),
            scale_px(104, dpi));
    }
    metrics.width = scale_px(316, dpi);
    return metrics;
}

int popup_row_height(TrayMenuEntryKind kind, const Win32TrayPopupMetrics& metrics) {
    if (tray_popup_entry_is_header(kind)) return metrics.header_height;
    if (kind == TrayMenuEntryKind::Separator) return metrics.separator_height;
    if (tray_popup_entry_is_fixed_action(kind)) return metrics.action_height;
    return metrics.item_height;
}

std::unique_ptr<Win32TrayPopupWindow> make_popup_window_state(
    Win32TrayPopupController* controller,
    bool is_submenu,
    std::vector<TrayPopupRow> rows,
    const Win32TrayPopupMetrics& metrics) {
    auto state = std::make_unique<Win32TrayPopupWindow>();
    state->controller = controller;
    state->is_submenu = is_submenu;
    state->metrics = metrics;
    state->font = create_popup_font(metrics.dpi);
    int top = 0;
    for (auto& row : rows) {
        Win32TrayPopupRow view;
        view.model = std::move(row);
        view.top = top;
        view.height = popup_row_height(view.model.entry.kind, metrics);
        top += view.height;
        state->rows.push_back(std::move(view));
    }
    state->content_height = top;
    return state;
}

RECT monitor_work_area(POINT pt) {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    HMONITOR monitor = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (::GetMonitorInfoW(monitor, &info)) return info.rcWork;
    return RECT{0, 0, ::GetSystemMetrics(SM_CXSCREEN), ::GetSystemMetrics(SM_CYSCREEN)};
}

int max_popup_scroll(const Win32TrayPopupWindow& state) {
    const int visible_content = std::max(
        0,
        state.viewport_height - state.metrics.outer_padding * 2);
    return std::max(0, state.content_height - visible_content);
}

void set_popup_scroll(Win32TrayPopupWindow& state, int next) {
    const int clamped = std::clamp(next, 0, max_popup_scroll(state));
    if (clamped == state.scroll_offset) return;
    state.scroll_offset = clamped;
    if (state.hwnd) ::InvalidateRect(state.hwnd, nullptr, FALSE);
}

void ensure_popup_row_visible(Win32TrayPopupWindow& state, int index) {
    if (index < 0 || index >= static_cast<int>(state.rows.size())) return;
    const auto& row = state.rows[static_cast<std::size_t>(index)];
    const int visible_height = std::max(
        0,
        state.viewport_height - state.metrics.outer_padding * 2);
    if (row.top < state.scroll_offset) {
        set_popup_scroll(state, row.top);
    } else if (row.top + row.height > state.scroll_offset + visible_height) {
        set_popup_scroll(state, row.top + row.height - visible_height);
    }
}

int popup_row_at(const Win32TrayPopupWindow& state, int x, int y) {
    if (x < 0 || x >= state.metrics.width) return -1;
    const int content_y = y - state.metrics.outer_padding + state.scroll_offset;
    for (std::size_t i = 0; i < state.rows.size(); ++i) {
        const auto& row = state.rows[i];
        if (content_y >= row.top && content_y < row.top + row.height) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void fill_rounded_rect(HDC hdc, const RECT& rect, int radius, COLORREF color) {
    HRGN region = ::CreateRoundRectRgn(
        rect.left,
        rect.top,
        rect.right + 1,
        rect.bottom + 1,
        radius,
        radius);
    HBRUSH brush = ::CreateSolidBrush(color);
    if (region && brush) ::FillRgn(hdc, region, brush);
    if (brush) ::DeleteObject(brush);
    if (region) ::DeleteObject(region);
}

void draw_popup_chevron(HDC hdc, const RECT& row_rect, const Win32TrayPopupMetrics& metrics) {
    const int center_x = row_rect.right - metrics.horizontal_padding;
    const int center_y = (row_rect.top + row_rect.bottom) / 2;
    const int half = scale_px(3, metrics.dpi);
    HPEN pen = ::CreatePen(PS_SOLID, std::max(1, scale_px(1, metrics.dpi)), RGB(76, 76, 76));
    HGDIOBJ old_pen = pen ? ::SelectObject(hdc, pen) : nullptr;
    ::MoveToEx(hdc, center_x - half, center_y - half, nullptr);
    ::LineTo(hdc, center_x, center_y);
    ::LineTo(hdc, center_x - half, center_y + half);
    if (old_pen) ::SelectObject(hdc, old_pen);
    if (pen) ::DeleteObject(pen);
}

void paint_tray_popup(Win32TrayPopupWindow& state) {
    PAINTSTRUCT ps{};
    HDC target = ::BeginPaint(state.hwnd, &ps);
    if (!target) return;

    RECT client{};
    ::GetClientRect(state.hwnd, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    HDC buffer = ::CreateCompatibleDC(target);
    HBITMAP bitmap = buffer ? ::CreateCompatibleBitmap(target, width, height) : nullptr;
    HGDIOBJ old_bitmap = bitmap ? ::SelectObject(buffer, bitmap) : nullptr;
    HDC draw = buffer && bitmap ? buffer : target;

    HBRUSH background = ::CreateSolidBrush(RGB(255, 255, 255));
    ::FillRect(draw, &client, background);
    ::DeleteObject(background);

    HFONT font = state.font
        ? state.font
        : static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
    HGDIOBJ old_font = font ? ::SelectObject(draw, font) : nullptr;
    const int old_bk_mode = ::SetBkMode(draw, TRANSPARENT);

    for (std::size_t i = 0; i < state.rows.size(); ++i) {
        const auto& row = state.rows[i];
        RECT row_rect{
            0,
            state.metrics.outer_padding + row.top - state.scroll_offset,
            width,
            state.metrics.outer_padding + row.top + row.height - state.scroll_offset,
        };
        if (row_rect.bottom <= 0 || row_rect.top >= height) continue;

        const auto kind = row.model.entry.kind;
        if (kind == TrayMenuEntryKind::Separator) {
            const int y = (row_rect.top + row_rect.bottom) / 2;
            HPEN pen = ::CreatePen(PS_SOLID, 1, RGB(225, 230, 238));
            HGDIOBJ old_pen = pen ? ::SelectObject(draw, pen) : nullptr;
            ::MoveToEx(draw, 0, y, nullptr);
            ::LineTo(draw, width, y);
            if (old_pen) ::SelectObject(draw, old_pen);
            if (pen) ::DeleteObject(pen);
            continue;
        }

        const bool selectable = tray_popup_entry_is_selectable(kind);
        if (selectable && state.hovered_index == static_cast<int>(i)) {
            HBRUSH hover = ::CreateSolidBrush(RGB(243, 243, 243));
            ::FillRect(draw, &row_rect, hover);
            ::DeleteObject(hover);
        }

        RECT text_rect = row_rect;
        text_rect.left += state.metrics.horizontal_padding;
        text_rect.right -= state.metrics.horizontal_padding;

        const std::wstring label = utf8_to_wide(row.model.entry.label);
        if (tray_popup_entry_is_header(kind)) {
            ::SetTextColor(draw, RGB(108, 101, 97));
            ::DrawTextW(draw,
                        label.c_str(),
                        static_cast<int>(label.size()),
                        &text_rect,
                        DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
            continue;
        }

        if (tray_popup_entry_is_session(kind)) {
            const std::wstring title = utf8_to_wide(
                row.model.entry.title.empty() ? row.model.entry.label : row.model.entry.title);
            const std::wstring subtitle = utf8_to_wide(row.model.entry.subtitle);
            RECT title_rect = text_rect;
            if (!subtitle.empty() && state.metrics.right_column_width > 0) {
                RECT subtitle_rect = text_rect;
                subtitle_rect.left = std::max(
                    subtitle_rect.left,
                    subtitle_rect.right - state.metrics.right_column_width);
                ::SetTextColor(draw, RGB(82, 73, 68));
                ::DrawTextW(draw,
                            subtitle.c_str(),
                            static_cast<int>(subtitle.size()),
                            &subtitle_rect,
                            DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_END_ELLIPSIS | DT_NOPREFIX);
                title_rect.right = std::max(
                    title_rect.left,
                    subtitle_rect.left - state.metrics.text_gap);
            }
            ::SetTextColor(draw, RGB(28, 28, 28));
            ::DrawTextW(draw,
                        title.c_str(),
                        static_cast<int>(title.size()),
                        &title_rect,
                        DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
            continue;
        }

        ::SetTextColor(draw, RGB(28, 28, 28));
        if (kind == TrayMenuEntryKind::MoreSubmenuRoot) {
            text_rect.right -= scale_px(16, state.metrics.dpi);
        }
        ::DrawTextW(draw,
                    label.c_str(),
                    static_cast<int>(label.size()),
                    &text_rect,
                    DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (kind == TrayMenuEntryKind::MoreSubmenuRoot) {
            draw_popup_chevron(draw, row_rect, state.metrics);
        }
    }

    const int max_scroll = max_popup_scroll(state);
    if (max_scroll > 0) {
        const int track_top = scale_px(8, state.metrics.dpi);
        const int track_height = std::max(1, height - track_top * 2);
        const int full_height = state.content_height + state.metrics.outer_padding * 2;
        const int thumb_height = std::clamp(
            state.viewport_height * track_height / std::max(1, full_height),
            scale_px(18, state.metrics.dpi),
            track_height);
        const int thumb_top = track_top +
            state.scroll_offset * (track_height - thumb_height) / max_scroll;
        RECT thumb{
            width - scale_px(4, state.metrics.dpi),
            thumb_top,
            width - scale_px(2, state.metrics.dpi),
            thumb_top + thumb_height,
        };
        fill_rounded_rect(draw, thumb, scale_px(2, state.metrics.dpi), RGB(190, 190, 190));
    }

    ::SetBkMode(draw, old_bk_mode);
    if (old_font) ::SelectObject(draw, old_font);
    if (buffer && bitmap) {
        ::BitBlt(target, 0, 0, width, height, buffer, 0, 0, SRCCOPY);
    }
    if (old_bitmap) ::SelectObject(buffer, old_bitmap);
    if (bitmap) ::DeleteObject(bitmap);
    if (buffer) ::DeleteDC(buffer);
    ::EndPaint(state.hwnd, &ps);
}

void apply_popup_window_shape(Win32TrayPopupWindow& state) {
    if (!state.hwnd) return;
    RECT client{};
    ::GetClientRect(state.hwnd, &client);
    HRGN region = ::CreateRoundRectRgn(
        0,
        0,
        client.right + 1,
        client.bottom + 1,
        state.metrics.corner_radius,
        state.metrics.corner_radius);
    if (region) {
        if (!::SetWindowRgn(state.hwnd, region, TRUE)) {
            ::DeleteObject(region);
        }
    }

    if (HMODULE dwm = ::LoadLibraryW(L"dwmapi.dll")) {
        using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
        auto set_attribute = reinterpret_cast<DwmSetWindowAttributeFn>(
            ::GetProcAddress(dwm, "DwmSetWindowAttribute"));
        if (set_attribute) {
            const DWORD rounded_preference = 2; // DWMWCP_ROUND
            set_attribute(state.hwnd, 33, &rounded_preference, sizeof(rounded_preference));
            const DWORD no_border = 0xFFFFFFFEu; // DWMWA_COLOR_NONE
            set_attribute(state.hwnd, 34, &no_border, sizeof(no_border));
        }
        ::FreeLibrary(dwm);
    }
}

bool create_popup_hwnd(Win32TrayPopupWindow& state,
                       HWND owner,
                       int x,
                       int y,
                       int height,
                       bool no_activate) {
    state.viewport_height = height;
    const DWORD ex_style = WS_EX_TOOLWINDOW | WS_EX_TOPMOST |
        (no_activate ? WS_EX_NOACTIVATE : 0);
    state.hwnd = ::CreateWindowExW(
        ex_style,
        kTrayPopupWndClass,
        L"ACECode Tray Menu",
        WS_POPUP,
        x,
        y,
        state.metrics.width,
        height,
        owner,
        nullptr,
        ::GetModuleHandleW(nullptr),
        &state);
    if (!state.hwnd) return false;
    apply_popup_window_shape(state);
    return true;
}

void destroy_popup_window(std::unique_ptr<Win32TrayPopupWindow>& state) {
    if (!state) return;
    if (state->hwnd && ::IsWindow(state->hwnd)) {
        HWND hwnd = state->hwnd;
        state->hwnd = nullptr;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        ::DestroyWindow(hwnd);
    }
    state.reset();
}

void close_tray_submenu() {
    if (!g_tray_popup) return;
    destroy_popup_window(g_tray_popup->submenu_window);
    g_tray_popup->keyboard_in_submenu = false;
    g_tray_popup->main_more_index = -1;
    if (g_tray_popup->main_window && g_tray_popup->main_window->hwnd) {
        ::InvalidateRect(g_tray_popup->main_window->hwnd, nullptr, FALSE);
    }
}

void close_tray_popup() {
    auto controller = std::move(g_tray_popup);
    if (!controller) return;
    controller->closing = true;
    destroy_popup_window(controller->submenu_window);
    destroy_popup_window(controller->main_window);
}

void activate_tray_popup_command(unsigned command_id) {
    if (!g_tray_popup || command_id == 0) return;
    TrayMenuLayout layout = g_tray_popup->layout;
    close_tray_popup();
    dispatch_menu_command(layout, command_id);
}

void update_popup_hover(Win32TrayPopupWindow& state, int index) {
    if (index >= 0 && index < static_cast<int>(state.rows.size()) &&
        !tray_popup_entry_is_selectable(
            state.rows[static_cast<std::size_t>(index)].model.entry.kind)) {
        index = -1;
    }
    if (state.hovered_index == index) return;
    state.hovered_index = index;
    if (state.hwnd) ::InvalidateRect(state.hwnd, nullptr, FALSE);
}

bool move_popup_selection(Win32TrayPopupWindow& state, int step) {
    if (state.rows.empty() || step == 0) return false;
    int index = state.hovered_index;
    for (std::size_t count = 0; count < state.rows.size(); ++count) {
        if (index < 0) {
            index = step > 0 ? 0 : static_cast<int>(state.rows.size()) - 1;
        } else {
            index = (index + step + static_cast<int>(state.rows.size())) %
                    static_cast<int>(state.rows.size());
        }
        if (tray_popup_entry_is_selectable(
                state.rows[static_cast<std::size_t>(index)].model.entry.kind)) {
            update_popup_hover(state, index);
            ensure_popup_row_visible(state, index);
            return true;
        }
    }
    return false;
}

bool open_tray_submenu(Win32TrayPopupWindow& main_window,
                       int row_index,
                       bool for_keyboard) {
    if (!g_tray_popup || main_window.is_submenu ||
        row_index < 0 || row_index >= static_cast<int>(main_window.rows.size())) {
        return false;
    }
    const auto& root = main_window.rows[static_cast<std::size_t>(row_index)];
    if (!tray_popup_row_has_submenu(root.model)) return false;

    if (g_tray_popup->submenu_window &&
        g_tray_popup->main_more_index == row_index) {
        if (for_keyboard) {
            g_tray_popup->keyboard_in_submenu = true;
            if (g_tray_popup->submenu_window->hovered_index < 0) {
                move_popup_selection(*g_tray_popup->submenu_window, 1);
            }
        }
        return true;
    }

    close_tray_submenu();
    auto rows = build_tray_popup_submenu_rows(root.model);
    auto submenu = make_popup_window_state(
        g_tray_popup.get(), true, std::move(rows), main_window.metrics);

    RECT main_rect{};
    ::GetWindowRect(main_window.hwnd, &main_rect);
    POINT monitor_point{main_rect.left, main_rect.top};
    const RECT work = monitor_work_area(monitor_point);
    const int margin = scale_px(8, main_window.metrics.dpi);
    const int gap = scale_px(6, main_window.metrics.dpi);
    const int desired_height = submenu->content_height + submenu->metrics.outer_padding * 2;
    const int work_left = static_cast<int>(work.left);
    const int work_top = static_cast<int>(work.top);
    const int work_right = static_cast<int>(work.right);
    const int work_bottom = static_cast<int>(work.bottom);
    const int max_height = std::max(
        submenu->metrics.item_height + submenu->metrics.outer_padding * 2,
        work_bottom - work_top - margin * 2);
    const int height = std::min(desired_height, max_height);

    int x = main_rect.right + gap;
    if (x + submenu->metrics.width > work_right - margin) {
        x = main_rect.left - gap - submenu->metrics.width;
    }
    x = std::clamp(x, work_left + margin, work_right - margin - submenu->metrics.width);
    int y = main_rect.top + main_window.metrics.outer_padding +
        root.top - main_window.scroll_offset;
    y = std::clamp(y, work_top + margin, work_bottom - margin - height);

    if (!create_popup_hwnd(*submenu, main_window.hwnd, x, y, height, true)) {
        return false;
    }
    g_tray_popup->main_more_index = row_index;
    g_tray_popup->keyboard_in_submenu = for_keyboard;
    g_tray_popup->submenu_window = std::move(submenu);
    ::SetWindowPos(
        g_tray_popup->submenu_window->hwnd,
        HWND_TOPMOST,
        x,
        y,
        g_tray_popup->submenu_window->metrics.width,
        height,
        SWP_SHOWWINDOW | SWP_NOACTIVATE);
    ::UpdateWindow(g_tray_popup->submenu_window->hwnd);
    if (for_keyboard) {
        move_popup_selection(*g_tray_popup->submenu_window, 1);
    }
    ::InvalidateRect(main_window.hwnd, nullptr, FALSE);
    return true;
}

void handle_popup_keyboard(WPARAM key) {
    if (!g_tray_popup || !g_tray_popup->main_window) return;
    if (key == VK_ESCAPE) {
        close_tray_popup();
        return;
    }

    Win32TrayPopupWindow* target = g_tray_popup->keyboard_in_submenu &&
            g_tray_popup->submenu_window
        ? g_tray_popup->submenu_window.get()
        : g_tray_popup->main_window.get();
    if (!target) return;

    if (key == VK_UP || key == VK_DOWN) {
        move_popup_selection(*target, key == VK_DOWN ? 1 : -1);
        return;
    }
    if (key == VK_LEFT && target->is_submenu) {
        close_tray_submenu();
        return;
    }
    if (target->hovered_index < 0 ||
        target->hovered_index >= static_cast<int>(target->rows.size())) {
        if (key == VK_RETURN || key == VK_RIGHT) move_popup_selection(*target, 1);
        return;
    }

    const auto& row = target->rows[static_cast<std::size_t>(target->hovered_index)];
    if (row.model.entry.kind == TrayMenuEntryKind::MoreSubmenuRoot) {
        if (key == VK_RETURN || key == VK_RIGHT) {
            open_tray_submenu(*target, target->hovered_index, true);
        }
        return;
    }
    if (key == VK_RETURN) {
        activate_tray_popup_command(row.model.entry.id);
    }
}

LRESULT CALLBACK tray_popup_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        auto* state = create
            ? static_cast<Win32TrayPopupWindow*>(create->lpCreateParams)
            : nullptr;
        if (!state) return FALSE;
        state->hwnd = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        return TRUE;
    }

    auto* state = reinterpret_cast<Win32TrayPopupWindow*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!state) return ::DefWindowProcW(hwnd, msg, wparam, lparam);

    switch (msg) {
        case WM_ERASEBKGND:
            return TRUE;
        case WM_PAINT:
            paint_tray_popup(*state);
            return 0;
        case WM_MOUSEACTIVATE:
            return state->is_submenu ? MA_NOACTIVATE : MA_ACTIVATE;
        case WM_ACTIVATE:
            if (!state->is_submenu && LOWORD(wparam) == WA_INACTIVE) {
                HWND next = reinterpret_cast<HWND>(lparam);
                HWND submenu = g_tray_popup && g_tray_popup->submenu_window
                    ? g_tray_popup->submenu_window->hwnd
                    : nullptr;
                if (!submenu || next != submenu) {
                    ::PostMessageW(hwnd, kTrayPopupDismissMessage, 0, 0);
                }
            }
            return 0;
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT track{};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = hwnd;
            ::TrackMouseEvent(&track);
            const int x = static_cast<int>(static_cast<short>(LOWORD(lparam)));
            const int y = static_cast<int>(static_cast<short>(HIWORD(lparam)));
            const int index = popup_row_at(*state, x, y);
            update_popup_hover(*state, index);
            if (state->is_submenu) {
                if (g_tray_popup) g_tray_popup->keyboard_in_submenu = true;
            } else if (index >= 0 &&
                       state->rows[static_cast<std::size_t>(index)].model.entry.kind ==
                           TrayMenuEntryKind::MoreSubmenuRoot) {
                open_tray_submenu(*state, index, false);
            } else if (index >= 0) {
                close_tray_submenu();
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            if (!state->is_submenu && g_tray_popup && g_tray_popup->submenu_window) {
                update_popup_hover(*state, g_tray_popup->main_more_index);
            } else {
                update_popup_hover(*state, -1);
            }
            return 0;
        case WM_LBUTTONUP: {
            const int x = static_cast<int>(static_cast<short>(LOWORD(lparam)));
            const int y = static_cast<int>(static_cast<short>(HIWORD(lparam)));
            const int index = popup_row_at(*state, x, y);
            if (index < 0 || index >= static_cast<int>(state->rows.size())) return 0;
            const auto& row = state->rows[static_cast<std::size_t>(index)];
            if (!tray_popup_entry_is_selectable(row.model.entry.kind)) return 0;
            if (row.model.entry.kind == TrayMenuEntryKind::MoreSubmenuRoot) {
                open_tray_submenu(*state, index, false);
            } else {
                activate_tray_popup_command(row.model.entry.id);
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
            const int step = state->metrics.item_height * 3;
            set_popup_scroll(
                *state,
                state->scroll_offset - (delta / WHEEL_DELTA) * step);
            return 0;
        }
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS | DLGC_WANTALLKEYS;
        case WM_KEYDOWN:
            if (!state->is_submenu) handle_popup_keyboard(wparam);
            return 0;
        case WM_CLOSE:
        case kTrayPopupDismissMessage:
            close_tray_popup();
            return 0;
        case WM_NCDESTROY:
            state->hwnd = nullptr;
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            break;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool register_tray_popup_class(HINSTANCE hinst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DROPSHADOW;
    wc.hInstance = hinst;
    wc.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = kTrayPopupWndClass;
    wc.lpfnWndProc = tray_popup_wnd_proc;
    if (::RegisterClassExW(&wc)) return true;
    return ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool show_custom_tray_popup(HWND owner,
                            const TrayMenuLayout& layout,
                            POINT anchor) {
    HINSTANCE hinst = ::GetModuleHandleW(nullptr);
    if (!register_tray_popup_class(hinst)) return false;

    auto controller = std::make_unique<Win32TrayPopupController>();
    controller->layout = layout;
    const int dpi = point_dpi(anchor);
    const Win32TrayPopupMetrics metrics = compute_popup_metrics(layout, dpi);
    auto rows = build_tray_popup_rows(layout);
    auto main_window = make_popup_window_state(
        controller.get(), false, std::move(rows), metrics);

    const RECT work = monitor_work_area(anchor);
    const int margin = scale_px(8, dpi);
    const int gap = scale_px(6, dpi);
    const int desired_height = main_window->content_height + metrics.outer_padding * 2;
    const int work_left = static_cast<int>(work.left);
    const int work_top = static_cast<int>(work.top);
    const int work_right = static_cast<int>(work.right);
    const int work_bottom = static_cast<int>(work.bottom);
    const int max_height = std::max(
        metrics.action_height + metrics.outer_padding * 2,
        work_bottom - work_top - margin * 2);
    const int height = std::min(desired_height, max_height);

    int x = anchor.x - metrics.width;
    if (x < work_left + margin && anchor.x + gap + metrics.width <= work_right - margin) {
        x = anchor.x + gap;
    }
    x = std::clamp(x, work_left + margin, work_right - margin - metrics.width);
    int y = anchor.y - height - gap;
    if (y < work_top + margin) y = anchor.y + gap;
    y = std::clamp(y, work_top + margin, work_bottom - margin - height);

    if (!create_popup_hwnd(*main_window, owner, x, y, height, false)) return false;
    controller->main_window = std::move(main_window);
    g_tray_popup = std::move(controller);

    ::SetForegroundWindow(owner);
    ::SetWindowPos(
        g_tray_popup->main_window->hwnd,
        HWND_TOPMOST,
        x,
        y,
        metrics.width,
        height,
        SWP_SHOWWINDOW);
    ::SetForegroundWindow(g_tray_popup->main_window->hwnd);
    ::SetFocus(g_tray_popup->main_window->hwnd);
    ::UpdateWindow(g_tray_popup->main_window->hwnd);
    return true;
}

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

// 自定义 popup 创建失败时保留一个最小原生菜单兜底,保证 session/action 命令
// 仍可用。正常路径不进入这里。
void append_layout_to_native_fallback(HMENU menu, const TrayMenuLayout& layout) {
    HMENU current_more_submenu = nullptr;
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
                current_more_submenu = nullptr;
                break;
            case TrayMenuEntryKind::MoreSubmenuRoot: {
                current_more_submenu = ::CreatePopupMenu();
                std::wstring w = utf8_to_wide(e.label);
                ::AppendMenuW(menu,
                              MF_POPUP | MF_STRING,
                              reinterpret_cast<UINT_PTR>(current_more_submenu),
                              w.c_str());
                break;
            }
            case TrayMenuEntryKind::MoreSubmenuItem: {
                if (!current_more_submenu) current_more_submenu = ::CreatePopupMenu();
                std::wstring w = utf8_to_wide(e.label);
                ::AppendMenuW(current_more_submenu, MF_STRING, e.id, w.c_str());
                break;
            }
        }
    }
    // 子菜单的句柄交给 menu 拥有,DestroyMenu 时连带回收。
}

void show_native_fallback_menu(HWND hwnd,
                               const TrayMenuLayout& layout,
                               POINT pt) {
    HMENU menu = ::CreatePopupMenu();
    if (!menu) return;
    append_layout_to_native_fallback(menu, layout);
    ::SetForegroundWindow(hwnd);
    UINT cmd = ::TrackPopupMenu(menu,
                                TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                                pt.x, pt.y, 0, hwnd, nullptr);
    ::PostMessageW(hwnd, WM_NULL, 0, 0);
    ::DestroyMenu(menu);
    dispatch_menu_command(layout, cmd);
}

void show_context_menu(HWND hwnd) {
    close_tray_popup();
    TrayMenuLayout layout = compute_menu_layout(snapshot_payload());
    POINT pt{};
    ::GetCursorPos(&pt);
    if (!show_custom_tray_popup(hwnd, layout, pt)) {
        LOG_WARN("[desktop] tray: custom popup unavailable, using native fallback");
        show_native_fallback_menu(hwnd, layout, pt);
    }
}

LRESULT CALLBACK tray_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == g_tray_callback_msg && g_tray_callback_msg != 0) {
        // tray icon 事件。lparam 低 WORD 是事件类型
        // (WM_LBUTTONUP / WM_RBUTTONUP 等)。具体见 Shell_NotifyIcon 文档。
        UINT event = LOWORD(lparam);
        switch (event) {
            case WM_LBUTTONUP:
            case WM_LBUTTONDBLCLK:
                // 双击与单击都路由到 on_show — bring_window_foreground 是幂等的,
                // 双击连发两次 SetForegroundWindow 没有观感差异。
                close_tray_popup();
                if (g_on_show) g_on_show();
                return 0;
            case WM_CONTEXTMENU:
            case WM_RBUTTONUP:
                show_context_menu(hwnd);
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

    // 真实的隐藏顶层窗口,**不能**用 HWND_MESSAGE(message-only):
    // message-only 窗口永远没有前台资格,show_context_menu 里的
    // SetForegroundWindow 会静默失败,TrackPopupMenu 的菜单 z-order 输给
    // 置顶的任务栏 —— 表现为「右键菜单有时被任务栏挡住/点外面不消失」,
    // 是否复现取决于当时进程里有没有别的可见前台窗口兜底(主窗口
    // close-to-tray 隐藏后必现)。Chromium status_icon_win / Qt
    // QSystemTrayIconSys 同样用真实隐藏窗口。无 WS_VISIBLE 永不显示;
    // WS_EX_TOOLWINDOW 兜底保证它绝不出现在任务栏/Alt+Tab。
    g_tray_window = ::CreateWindowExW(
        WS_EX_TOOLWINDOW,
        kTrayWndClass,
        L"ACECode Tray",
        WS_POPUP,
        0, 0, 0, 0,
        nullptr,
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
    close_tray_popup();
    if (g_icon_added) {
        ::Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_icon_added = false;
    }
    if (g_tray_window) {
        ::DestroyWindow(g_tray_window);
        g_tray_window = nullptr;
    }
    HINSTANCE hinst = ::GetModuleHandleW(nullptr);
    ::UnregisterClassW(kTrayPopupWndClass, hinst);
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

void mac_bring_window_foreground(void* ns_window) {
    // 菜单/状态栏回调都在主线程,可直接打 AppKit。
    [NSApp activateIgnoringOtherApps:YES];
    NSWindow* win = static_cast<NSWindow*>(ns_window);
    if (!win) return;
    if ([win isMiniaturized]) [win deminiaturize:nil];
    [win makeKeyAndOrderFront:nil];
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
