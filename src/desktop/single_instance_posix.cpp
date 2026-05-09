// POSIX 实现:`flock` 在 `~/.acecode/run/acecode-desktop.lock` 拿独占锁。
//
// 设计见 single_instance.hpp 头注。窗口拉前 v1 留 stub:Linux 桌面壳尚未发布,
// 等到接 D-Bus(GTK/Qt)或 macOS NSDistributedNotificationCenter 时再补真实
// 跨进程 IPC 通道。
//
// 只在非 Windows 平台编译;_WIN32 走 single_instance_win.cpp。

#include "single_instance.hpp"

#ifndef _WIN32

#include "../utils/logger.hpp"
#include "../utils/paths.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <sys/file.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace acecode::desktop {

namespace {

std::string default_lock_path() {
    fs::path run_dir = fs::path(acecode::get_acecode_dir()) / "run";
    std::error_code ec;
    fs::create_directories(run_dir, ec);
    return (run_dir / "acecode-desktop.lock").string();
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
    // POSIX v1 stub:没有跨进程 "拉前" 通道。调用方收到 false 应当 exit,让用户
    // 自己点系统托盘 / 切窗(若已经有 desktop 实例的话)。Linux/macOS 桌面壳
    // 落地时再补真实 IPC。
    LOG_WARN("[desktop] focus_existing_instance: not implemented on POSIX (v1 stub)");
    return false;
}

} // namespace acecode::desktop

#endif // !_WIN32
