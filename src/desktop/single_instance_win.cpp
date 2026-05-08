// Windows 实现:`CreateMutexW(Local\\...)` + `PostMessageW` 给已有 host window。
// 设计见 single_instance.hpp 头注。

#include "single_instance.hpp"

#ifdef _WIN32

#include "../utils/logger.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>

namespace acecode::desktop {

namespace {

// `Local\\` 前缀 = 当前 session(每用户登录一份);"-v1" 留版本号让以后不兼容
// 改 schema 时换名而不和老进程撞锁。
constexpr wchar_t kMutexName[] = L"Local\\ACECode-Desktop-Singleton-v1";

// 同名注册消息 — 多个进程拿到同一个 UINT,且本会话唯一。host_window_proc 用
// 这个值检测 "另一个 desktop 让我拉前"。不同 Windows 重启间这个值会变 → OK,
// 我们也只在同一会话内通讯。
constexpr wchar_t kFocusMsgName[] = L"ACECode_FocusExistingInstance_v1";

UINT focus_existing_msg_id() {
    static UINT id = ::RegisterWindowMessageW(kFocusMsgName);
    return id;
}

} // namespace

// 把消息 ID 暴露给 web_host.cpp(同 .exe 内部链接 — 不进 hpp 是为了不让其它
// TU 误用)。host_window_proc 在 WndProc 里比较这个 ID。
UINT desktop_focus_existing_message_id() {
    return focus_existing_msg_id();
}

SingleInstance::SingleInstance() = default;

SingleInstance::~SingleInstance() {
    release();
}

bool SingleInstance::try_acquire() {
    if (acquired_) return true;
    HANDLE h = ::CreateMutexW(nullptr, /*bInitialOwner=*/TRUE, kMutexName);
    if (!h) {
        DWORD err = ::GetLastError();
        LOG_WARN("[desktop] single_instance: CreateMutexW failed, err=" + std::to_string(err));
        return false;
    }
    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        // 锁已被其它进程持有。把 handle close 掉,不污染对方的锁状态。
        ::CloseHandle(h);
        LOG_INFO("[desktop] single_instance: another instance is already running");
        return false;
    }
    native_handle_ = h;
    acquired_ = true;
    // 提前 register focus msg id,确保后续 host_window_proc 能在第一次 WM 派发时
    // 拿到一致的 UINT(不会因为 race 出现 RegisterWindowMessage 时序问题)。
    focus_existing_msg_id();
    LOG_INFO("[desktop] single_instance: lock acquired");
    return true;
}

void SingleInstance::release() {
    if (!acquired_) return;
    HANDLE h = static_cast<HANDLE>(native_handle_);
    if (h) {
        // ReleaseMutex 让等待方看见 WAIT_OBJECT_0,我们当前没人 wait,但保持
        // 良好风格。CloseHandle 会真正回收。
        ::ReleaseMutex(h);
        ::CloseHandle(h);
    }
    native_handle_ = nullptr;
    acquired_ = false;
}

bool focus_existing_instance() {
    UINT msg = focus_existing_msg_id();
    if (msg == 0) {
        LOG_WARN("[desktop] focus_existing_instance: RegisterWindowMessageW returned 0");
        return false;
    }
    // 已有 host window 的窗口类是 ACECodeDesktopHostWindow(web_host.cpp 注册)。
    HWND existing = ::FindWindowW(L"ACECodeDesktopHostWindow", nullptr);
    if (existing) {
        ::PostMessageW(existing, msg, 0, 0);
        LOG_INFO("[desktop] focus_existing_instance: posted focus msg to existing window");
        return true;
    }
    // 窗口可能还没创建(对方刚启动 / 处于 splash 阶段)→ 广播,任何同消息 ID
    // 的 hidden window(包括 tray message-only)也能收到。我们 tray_wnd_proc
    // 不处理这个 msg,但它至少不会崩。
    DWORD recipients = BSM_APPLICATIONS;
    LRESULT r = ::BroadcastSystemMessageW(BSF_POSTMESSAGE | BSF_IGNORECURRENTTASK,
                                          &recipients, msg, 0, 0);
    if (r > 0) {
        LOG_INFO("[desktop] focus_existing_instance: broadcasted focus msg (no existing window found)");
        return true;
    }
    LOG_WARN("[desktop] focus_existing_instance: existing instance has no window yet, broadcast failed");
    return false;
}

} // namespace acecode::desktop

#endif // _WIN32
