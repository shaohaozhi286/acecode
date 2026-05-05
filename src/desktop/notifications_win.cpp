// Windows 桌面壳系统通知 — V1 气泡实现(Shell_NotifyIcon + NIIF_INFO)。
//
// 设计参见 openspec/changes/add-desktop-attention-notifications/design.md 决策 1。
// V2 future work:接 WinRT ToastNotificationManager + AUMID + 开始菜单 .lnk。

#include "notifications_win.hpp"

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
#include <string>

namespace acecode::desktop {

#ifdef _WIN32

// tray_icon_win.cpp 定义,在 acecode::desktop 命名空间下导出 tray icon 的
// NOTIFYICONDATA UID。气泡通知 piggyback 在同一 UID 上,所以两端必须共享。
UINT tray_icon_uid_value();

namespace {

std::mutex g_mu;
HWND g_tray_message_hwnd = nullptr;
bool g_initialized = false;
ClickHandler g_click_handler;

// 当前挂着的气泡 payload。Shell_NotifyIcon 一次只能挂一条,所以只保存最近一条。
// 当 NIN_BALLOONUSERCLICK 到达时拿这一条派发。WndProc 收到的事件不带 payload,
// 所以这种"全局当前挂着"的反查方式是必要的。
NotifyPayload g_active{};
bool g_active_valid = false;

// Shell_NotifyIconW szInfoTitle 上限 64 wide chars(含 \0),szInfo 上限 256。
// 这里按 wchar_t 长度算,UTF-8 转 wide 后超长就截断 + 添加 …。
std::wstring truncate_wide(std::wstring s, size_t limit_inclusive_null) {
    if (limit_inclusive_null == 0) return std::wstring();
    if (s.size() + 1 <= limit_inclusive_null) return s;
    // 留 4 个 wchar 给 "…\0",末尾追加 ellipsis
    if (limit_inclusive_null < 4) {
        s.resize(limit_inclusive_null - 1);
        return s;
    }
    s.resize(limit_inclusive_null - 2);  // 留 \0 和 …
    s.push_back(L'…');
    return s;
}

bool fill_balloon(NOTIFYICONDATAW& nid, const std::wstring& title, const std::wstring& body) {
    if (!g_tray_message_hwnd) return false;
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_tray_message_hwnd;
    nid.uID = tray_icon_uid_value();
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;

    auto title_w = truncate_wide(title, std::size(nid.szInfoTitle));
    auto body_w  = truncate_wide(body, std::size(nid.szInfo));
    wcsncpy_s(nid.szInfoTitle, title_w.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo,      body_w.c_str(),  _TRUNCATE);
    return true;
}

} // namespace

bool init_notifications(void* tray_message_hwnd) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!tray_message_hwnd) {
        LOG_WARN("[desktop] init_notifications: tray HWND null, notifications disabled");
        g_initialized = false;
        return false;
    }
    g_tray_message_hwnd = static_cast<HWND>(tray_message_hwnd);
    g_initialized = true;
    g_active_valid = false;
    LOG_INFO("[desktop] notifications: initialized (Shell_NotifyIcon balloon mode)");
    return true;
}

void set_click_handler(ClickHandler handler) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_click_handler = std::move(handler);
}

void show_notification(const NotifyPayload& payload) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_initialized) return;
    if (payload.title.empty() && payload.body.empty()) return;

    NOTIFYICONDATAW nid{};
    auto wtitle = acecode::utf8_to_wide(payload.title.empty() ? std::string("ACECode") : payload.title);
    auto wbody  = acecode::utf8_to_wide(payload.body);
    if (!fill_balloon(nid, wtitle, wbody)) return;

    // 把 payload 记到 g_active,等 WndProc 收到 NIN_BALLOONUSERCLICK 时反查。
    g_active = payload;
    g_active_valid = true;

    if (!::Shell_NotifyIconW(NIM_MODIFY, &nid)) {
        // 极偶发:tray icon 还没注册成功就被调到。降级 no-op,不抛错。
        LOG_WARN("[desktop] Shell_NotifyIconW NIM_MODIFY failed, dropping notification");
        g_active_valid = false;
    }
}

void on_balloon_clicked() {
    NotifyPayload payload;
    ClickHandler handler;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_active_valid) return;
        payload = g_active;
        handler = g_click_handler;
        g_active_valid = false;
    }
    if (handler) {
        handler(payload.id, payload.workspace_hash, payload.session_id);
    }
}

void shutdown_notifications() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_initialized = false;
    g_active_valid = false;
    g_tray_message_hwnd = nullptr;
    g_click_handler = nullptr;
}

#else // !_WIN32

bool init_notifications(void* /*tray_message_hwnd*/) { return false; }
void set_click_handler(ClickHandler /*handler*/) {}
void show_notification(const NotifyPayload& /*payload*/) {}
void on_balloon_clicked() {}
void shutdown_notifications() {}

#endif // _WIN32

} // namespace acecode::desktop
