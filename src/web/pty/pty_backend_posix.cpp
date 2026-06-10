// POSIX PTY 后端(openspec/changes/add-console-dock,design.md D3):
// 单层 forkpty(),无 fallback 链 — 伪终端是 Unix 内核原生设施。
// Linux: <pty.h> + util 库(glibc 2.34+ 已并入 libc);macOS: <util.h>。
#ifndef _WIN32

#include "pty_backend.hpp"

#include "../../utils/logger.hpp"

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <mutex>
#include <thread>

namespace acecode {

namespace {

class PosixPtyProcess : public PtyProcess {
public:
    explicit PosixPtyProcess(PtyCallbacks callbacks)
        : callbacks_(std::move(callbacks)) {}

    ~PosixPtyProcess() override { kill(); }

    PtyBackendKind kind() const override { return PtyBackendKind::PosixPty; }
    int pid() const override { return static_cast<int>(pid_); }

    void write(const std::string& data) override {
        std::lock_guard<std::mutex> lock(fd_mu_);
        if (stopped_.load() || master_fd_ < 0) return;
        size_t off = 0;
        while (off < data.size()) {
            ssize_t n = ::write(master_fd_, data.data() + off, data.size() - off);
            if (n <= 0) {
                if (errno == EINTR) continue;
                break;
            }
            off += static_cast<size_t>(n);
        }
    }

    void resize(int cols, int rows) override {
        std::lock_guard<std::mutex> lock(fd_mu_);
        if (stopped_.load() || master_fd_ < 0) return;
        struct winsize ws {};
        ws.ws_col = static_cast<unsigned short>(cols);
        ws.ws_row = static_cast<unsigned short>(rows);
        ioctl(master_fd_, TIOCSWINSZ, &ws);  // 内核给前台进程组发 SIGWINCH
    }

    void kill() override {
        bool expected = false;
        if (!stopped_.compare_exchange_strong(expected, true)) {
            join_reader();
            return;
        }
        if (pid_ > 0) ::kill(pid_, SIGKILL);
        close_master();
        join_reader();
    }

    bool spawn(const PtySpawnSpec& spec, std::string& error) {
        struct winsize ws {};
        ws.ws_col = static_cast<unsigned short>(spec.cols);
        ws.ws_row = static_cast<unsigned short>(spec.rows);

        pid_t pid = forkpty(&master_fd_, nullptr, nullptr, &ws);
        if (pid < 0) {
            error = std::string("forkpty failed: ") + strerror(errno);
            return false;
        }
        if (pid == 0) {
            // 子进程:slave 端已接到 0/1/2,内核 line discipline 就位。
            setenv("TERM", "xterm-256color", 1);
            if (!spec.cwd.empty() && chdir(spec.cwd.c_str()) != 0) {
                // cwd 不可进入时退回继承的目录,shell 仍可用。
            }
            execlp(spec.shell.c_str(), spec.shell.c_str(), nullptr);
            _exit(127);  // exec 失败
        }
        pid_ = pid;
        reader_ = std::thread([this] { reader_main(); });
        return true;
    }

private:
    void reader_main() {
        char buf[16 * 1024];
        for (;;) {
            ssize_t n = ::read(master_fd_, buf, sizeof(buf));
            if (n < 0 && errno == EINTR) continue;
            // n == 0(EOF)或 EIO(slave 端全关,Linux 惯例)= 进程结束。
            if (n <= 0) break;
            if (stopped_.load()) break;
            if (callbacks_.on_data) callbacks_.on_data(std::string(buf, static_cast<size_t>(n)));
        }
        int status = 0;
        int exit_code = 0;
        if (pid_ > 0 && waitpid(pid_, &status, 0) == pid_) {
            if (WIFEXITED(status)) exit_code = WEXITSTATUS(status);
            else if (WIFSIGNALED(status)) exit_code = 128 + WTERMSIG(status);
        }
        if (!stopped_.load() && callbacks_.on_exit) callbacks_.on_exit(exit_code);
    }

    void close_master() {
        std::lock_guard<std::mutex> lock(fd_mu_);
        if (master_fd_ >= 0) {
            ::close(master_fd_);  // 逼读线程退出阻塞 read
            master_fd_ = -1;
        }
    }

    void join_reader() {
        if (reader_.joinable()) reader_.join();
    }

    PtyCallbacks callbacks_;
    std::atomic<bool> stopped_{false};
    std::mutex fd_mu_;
    std::thread reader_;
    int master_fd_ = -1;
    pid_t pid_ = -1;
};

} // namespace

PtyBackendKind detect_pty_backend() { return PtyBackendKind::PosixPty; }

std::unique_ptr<PtyProcess> spawn_pty(PtyBackendKind kind,
                                      const PtySpawnSpec& spec,
                                      PtyCallbacks callbacks,
                                      std::string& error) {
    if (kind != PtyBackendKind::PosixPty) {
        error = "only the posix pty backend is available on this platform";
        return nullptr;
    }
    auto p = std::make_unique<PosixPtyProcess>(std::move(callbacks));
    if (!p->spawn(spec, error)) return nullptr;
    return p;
}

} // namespace acecode

#endif // !_WIN32
