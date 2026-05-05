#pragma once

// Windows 桌面壳的系统托盘图标 (NotifyIcon)。
//
// 见 openspec/changes/add-desktop-attention-notifications/。
//
// 注册:
//   1. 创建一个 hidden message-only 窗口(HWND_MESSAGE),WndProc 接 tray 事件
//   2. Shell_NotifyIconW(NIM_ADD, ...) 把图标加进通知区
//   3. 暴露 message-only HWND 给 notifications_win,让气泡通知 piggyback 上去
//
// 行为:
//   - 左键单击 → on_show()(把主窗口拉前)
//   - 右键 → 上下文菜单 "显示窗口" / "退出"
//   - 气泡点击 → 转发到 notifications_win::on_balloon_clicked()

#include <functional>

namespace acecode::desktop {

using TrayClickHandler = std::function<void()>;

// init 必须在主线程上调,在 WebHost::run() 之前。
//
// out_message_hwnd 必填,init 成功后写入 tray 的 message-only HWND(typed `void*`
// 让该 header 不必 include <windows.h>);失败时写 nullptr 并返回 false。
//
// 失败原因:RegisterClass / CreateWindow / Shell_NotifyIcon NIM_ADD 任一出错。
// 失败时主流程仍可继续,只是没有托盘图标。
bool init_tray_icon(TrayClickHandler on_show,
                    TrayClickHandler on_quit,
                    void** out_message_hwnd);

// 撤销注册 + 销毁 hidden window。进程退出前调,避免残留托盘 ghost icon。
// 注意 Win32 的 NIM_DELETE 经常需要 GetClassInfoEx + UnregisterClass 来彻底清理,
// 这里都做了。
void shutdown_tray_icon();

} // namespace acecode::desktop
