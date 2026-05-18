#pragma once

// 桌面壳的系统托盘图标。
//
// 见 openspec/changes/add-desktop-attention-notifications/。
// 扩展 Codex 风格菜单 + close-to-tray:openspec/changes/enhance-desktop-tray-menu/。
//
// Windows 注册:
//   1. 创建一个 hidden message-only 窗口(HWND_MESSAGE),WndProc 接 tray 事件
//   2. Shell_NotifyIconW(NIM_ADD, ...) 把图标加进通知区
//   3. 暴露 message-only HWND 给 notifications_win,让气泡通知 piggyback 上去。
//
// Linux 注册:
//   - 复用 WebKitGTK/GTK3 runtime,通过 GtkStatusIcon 安装图标和菜单。
//   - 不强制链接 GTK:Linux 后端运行时 dlopen,缺失时安全降级为无托盘。
//
// 行为:
//   - 左键单击 / 双击 → on_show()(把主窗口拉前)
//   - 右键 → Codex 风格上下文菜单(Pinned / Recent / 新建会话 / 打开 / 退出)
//   - 气泡点击 → 转发到 notifications_win::on_balloon_clicked()

#include <functional>
#include <string>
#include <vector>

namespace acecode::desktop {

using TrayClickHandler = std::function<void()>;

// 托盘菜单中的 session 项。subtitle 用于第二行小字(workspace 名 / summary,可空)。
struct TrayMenuItem {
    std::string session_id;
    std::string workspace_hash;
    std::string title;
    std::string subtitle;
};

// 由前端 aceDesktop_setTrayMenu 推送的菜单数据。native 缓存这份 payload,右键时
// 直接渲染,不再回查 daemon。pinned + recent 上限分别由 menu_layout 中的常量管。
struct TrayMenuPayload {
    std::string workspace_name;
    std::vector<TrayMenuItem> pinned;
    std::vector<TrayMenuItem> recent;
};

// session 项被点击时的 callback(workspace_hash + session_id)。
using TraySessionClickHandler =
    std::function<void(const std::string& workspace_hash, const std::string& session_id)>;

// init 必须在主线程上调,在 WebHost::run() 之前。
//
// out_message_hwnd 必填。Windows init 成功后写入 tray 的 message-only HWND
// (typed `void*` 让该 header 不必 include <windows.h>);Linux 写 nullptr。
// 失败时写 nullptr 并返回 false。
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

// Codex 风格菜单数据 + handler 注册。所有 setter 线程安全(内部 mutex)。
// 见 openspec/changes/enhance-desktop-tray-menu。

// 替换缓存的 tray menu payload。前端在 sessions / pinned / workspaceName 变化
// 时调一次,native 取快照渲染右键菜单。
void set_tray_menu_payload(TrayMenuPayload payload);

// 清空缓存,菜单退化为 "新建会话 / 打开 ACECode / 退出"(用于切换 workspace 时)。
void clear_tray_menu_payload();

// 注册三类菜单项的 click handler。任一为 nullptr 则该菜单项点击 no-op(不崩)。
void set_tray_session_click_handler(TraySessionClickHandler handler);
void set_tray_new_chat_handler(TrayClickHandler handler);
void set_tray_open_app_handler(TrayClickHandler handler);

} // namespace acecode::desktop
