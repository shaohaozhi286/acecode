#pragma once

// acecode-desktop 全局单例(per-user)。
//
// 设计:
//   - 每个用户同时只能跑一个 acecode-desktop,避免出现两个 webview 各自又拉起一份
//     daemon 子进程 / 各自 grab tray icon。
//   - 第二次启动时:不报错弹框,而是把已有那个窗口拉前 + 显示(类似 Slack / VS Code
//     的双击图标行为),然后自己 exit(0)。
//   - 锁 scope 是 per-user(不是 system-wide):多用户登录同一台机器时各自允许
//     一个 desktop 实例。
//
// 实现:
//   - Windows: `CreateMutexW(L"Local\\ACECode-Desktop-Singleton-v1")`(`Local\\`
//     前缀 = per-session,等价于 per-logged-in-user);第二次启动通过
//     `RegisterWindowMessageW` 拿一个全系统唯一的消息 ID,`PostMessageW` 给
//     已有 host window 让它把自己拉前。`web_host.cpp::host_window_proc` 收到
//     这条 msg → ShowWindow + SetForegroundWindow。
//   - POSIX:`flock` 在 `~/.acecode/run/acecode-desktop.lock` 上拿独占锁。
//     macOS 二次启动通过 NSDistributedNotificationCenter 拉前已有窗口;Linux
//     目前仍是 stub。
//
// 跨平台 boundary:
//   - `SingleInstance` 类与 `focus_existing_instance()` 自由函数是接口契约。
//   - `_win.cpp` / `_posix.cpp` 各自只编一份(由 #ifdef 守卫),CMake glob 全收。

#include <string>

namespace acecode::desktop {

// RAII 单例锁。析构时自动 release。仅一个进程能持有。
class SingleInstance {
public:
    SingleInstance();
    ~SingleInstance();

    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

    // 试图拿到单例锁。返回 true = 成功(我是唯一实例);false = 已有别的实例
    // 持锁(本进程应当 focus existing + exit)。可重入但只有第一次有效。
    bool try_acquire();

    // 是否当前持锁。
    bool acquired() const { return acquired_; }

    // 显式 release。析构会自动调,显式调允许测试覆盖锁释放后的再获取。
    void release();

private:
    bool acquired_ = false;
    void* native_handle_ = nullptr; // Windows: HANDLE;POSIX: 把 fd 装在 intptr_t 里
    std::string lock_path_;          // POSIX 用,Windows 留空
};

// 把 "已有 desktop 实例" 的主窗口拉到前台。
// 返回 true:已发送拉前请求(对方应该会响应)。
// 返回 false:平台不支持 / 找不到对方窗口。
//
// Windows 实现走 PostMessageW + RegisterWindowMessageW 的 system-wide msg ID,
// 已有进程的 host_window_proc 收到后 ShowWindow(SW_SHOW) + SetForegroundWindow。
//
// macOS 走分布式通知;Linux 当前 stub 返回 false,调用方应在 false 时直接 exit。
bool focus_existing_instance();

} // namespace acecode::desktop
