// POSIX 实现:`flock` 在 `~/.acecode/run/acecode-desktop.lock` 拿独占锁。
//
// 设计见 single_instance.hpp 头注。macOS 通过 distributed notification 唤醒
// 已有窗口;Linux 桌面壳尚未发布,窗口拉前仍留 D-Bus/GTK 后续实现。
//
// 只在非 Windows 平台编译;_WIN32 走 single_instance_win.cpp。

#include "single_instance.hpp"

#ifndef _WIN32

#include "../config/config.hpp"
#include "../utils/logger.hpp"
#include "../utils/utf8_path.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <sys/file.h>
#include <unistd.h>

#ifdef __APPLE__
#  include <CoreFoundation/CoreFoundation.h>
#endif

namespace fs = std::filesystem;

namespace acecode::desktop {

namespace {

std::string default_lock_path() {
    fs::path run_dir = acecode::path_from_utf8(acecode::get_run_dir());
    std::error_code ec;
    fs::create_directories(run_dir, ec);
    return acecode::path_to_utf8(run_dir / "acecode-desktop.lock");
}

// 把 fd 装在 native_handle_ 里(用 intptr_t cast 进 void*),与 Windows 端
// 不重复定义 native handle 类型。
intptr_t handle_to_fd(void* h) { return reinterpret_cast<intptr_t>(h); }
void*    fd_to_handle(intptr_t fd) { return reinterpret_cast<void*>(fd); }

} // namespace

SingleInstance::SingleInstance() = default;

SingleInstance::~SingleInstance() {
    release();
}

bool SingleInstance::try_acquire() {
    if (acquired_) return true;
    lock_path_ = default_lock_path();
    int fd = ::open(lock_path_.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) {
        LOG_WARN(std::string("[desktop] single_instance: open(") + lock_path_ +
                 ") failed: " + std::strerror(errno));
        return false;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        // EWOULDBLOCK / EAGAIN = 别的进程持有锁。其它 errno 视为环境异常,同样
        // 返回 false 让上层走 focus_existing_instance / exit 路径。
        ::close(fd);
        LOG_INFO(std::string("[desktop] single_instance: another instance holds ") +
                 lock_path_ + " (errno=" + std::to_string(errno) + ")");
        return false;
    }
    // flock 成功 — 我是第一个。fd 持续打开,close 时锁自动释放(POSIX flock 语义)。
    native_handle_ = fd_to_handle(static_cast<intptr_t>(fd));
    acquired_ = true;
    LOG_INFO("[desktop] single_instance: lock acquired at " + lock_path_);
    return true;
}

void SingleInstance::release() {
    if (!acquired_) return;
    int fd = static_cast<int>(handle_to_fd(native_handle_));
    if (fd >= 0) {
        ::flock(fd, LOCK_UN);
        ::close(fd);
    }
    native_handle_ = nullptr;
    acquired_ = false;
    // 故意不 unlink lock 文件 — 留下让下次启动复用,避免 TOCTOU。
}

bool focus_existing_instance() {
#ifdef __APPLE__
    CFNotificationCenterPostNotification(
        CFNotificationCenterGetDistributedCenter(),
        CFSTR("dev.acecode.desktop.focusExisting.v1"),
        nullptr,
        nullptr,
        true);
    LOG_INFO("[desktop] focus_existing_instance: posted macOS distributed focus notification");
    return true;
#else
    // POSIX v1 stub:Linux 还没有跨进程 "拉前" 通道。调用方收到 false 应当 exit,
    // 让用户自己点系统托盘 / 切窗(若已经有 desktop 实例的话)。
    LOG_WARN("[desktop] focus_existing_instance: not implemented on POSIX (v1 stub)");
    return false;
#endif
}

} // namespace acecode::desktop

#endif // !_WIN32
