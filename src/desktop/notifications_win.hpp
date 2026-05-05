#pragma once

// Windows 桌面壳的"系统通知"封装。
//
// 见 openspec/changes/add-desktop-attention-notifications/。
//
// V1 实现:Shell_NotifyIcon + NIIF_INFO 气泡(Win10/11 上系统会渲染成 toast-like
// 样式,Win 早期版本是经典气泡)。V2 计划接 WinRT ToastNotificationManager(需要
// AUMID + 开始菜单 .lnk 注册 + cppwinrt 集成),界面更现代,推到后续 PR。
//
// 与 tray_icon_win 的关系:气泡通知 piggyback 在 tray icon 的 NOTIFYICONDATA 上
// (`Shell_NotifyIconW(NIM_MODIFY, ... NIF_INFO)`),所以 init_notifications 必须
// 在 init_tray_icon 之后调,并把 tray 的 message-only HWND 传过来。

#include <functional>
#include <string>

namespace acecode::desktop {

// 通知 payload。`id` 用于在 Shell_NotifyIcon 模式下,通过 pending 表反查 click
// 时该弹谁的 (workspace, session) — 因为气泡只能挂当前一条,点击事件不带 payload。
// 调用方负责保证 id 唯一(可用 `notify-<seq>`)。
struct NotifyPayload {
    std::string id;              // 任意稳定字符串,前端透传
    std::string workspace_hash;  // 跳转用
    std::string session_id;      // 跳转用
    std::string title;           // 通知标题(气泡 NIIF szInfoTitle 上限 64,UTF-8 codepoint 计数)
    std::string body;            // 通知正文(气泡 szInfo 上限 256)
};

using ClickHandler = std::function<void(const std::string& id,
                                        const std::string& workspace_hash,
                                        const std::string& session_id)>;

// init 必须在 init_tray_icon 之后调。tray_message_hwnd 是 tray 的 message-only
// HWND,气泡复用它做 NOTIFYICONDATA::hWnd。失败 → 返回 false,后续 show_notification
// 静默 no-op。
bool init_notifications(void* tray_message_hwnd);

// 注册点击回调。允许 nullptr(等于关闭点击响应);多次调用以最后一次为准。
void set_click_handler(ClickHandler handler);

// 投递气泡通知。若 init 未成功或 payload 无效则 no-op。同一时间只能挂一条气泡,
// 调多次会被新的覆盖(Shell_NotifyIcon 行为)。
void show_notification(const NotifyPayload& payload);

// tray window 的 WndProc 收到 NIN_BALLOONUSERCLICK 时调这里,会反查 payload 并
// 调注册的 click_handler。无 pending payload 则静默忽略。
void on_balloon_clicked();

// 撤销注册 + 清 pending。进程退出前调。
void shutdown_notifications();

} // namespace acecode::desktop
