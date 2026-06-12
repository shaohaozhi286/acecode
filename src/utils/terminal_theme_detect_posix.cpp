#ifndef _WIN32

#include "terminal_theme_detect.hpp"
#include "logger.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

namespace acecode {

// ---- OSC 11 探测(POSIX) ----
// raw 模式写 \033]11;?\033\\ 到 stdout,select 200ms 超时读 stdin。

std::optional<std::string> probe_osc11_platform() {
    // stdin 必须是 tty 才能做 OSC 查询
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        return std::nullopt;
    }

    struct termios old_termios {};
    if (tcgetattr(STDIN_FILENO, &old_termios) != 0) {
        return std::nullopt;
    }

    // raw 模式:关闭 echo、canonical、signal
    struct termios raw = old_termios;
    raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO | ISIG);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
        return std::nullopt;
    }

    // 清空待读数据
    tcflush(STDIN_FILENO, TCIFLUSH);

    // 发送 OSC 11 查询
    const char query[] = "\033]11;?\033\\";
    // NOLINTNEXTLINE — write to stdout fd, not file stream
    (void)write(STDOUT_FILENO, query, sizeof(query) - 1);

    // 200ms 总超时读响应
    std::string response;
    struct timeval total_deadline {};
    gettimeofday(&total_deadline, nullptr);
    total_deadline.tv_usec += 200000;
    if (total_deadline.tv_usec >= 1000000) {
        total_deadline.tv_sec += 1;
        total_deadline.tv_usec -= 1000000;
    }

    while (true) {
        struct timeval now {};
        gettimeofday(&now, nullptr);
        long remaining_us = (total_deadline.tv_sec - now.tv_sec) * 1000000L +
                            (total_deadline.tv_usec - now.tv_usec);
        if (remaining_us <= 0) break;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        struct timeval tv {};
        tv.tv_sec  = remaining_us / 1000000L;
        tv.tv_usec = remaining_us % 1000000L;

        int ret = select(STDIN_FILENO + 1, &rfds, nullptr, nullptr, &tv);
        if (ret <= 0) break;

        char buf[128];
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n <= 0) break;
        response.append(buf, static_cast<std::size_t>(n));

        // 完整响应以 ST(\033\\) 或 BEL(\007) 结尾
        if (response.find("\033\\") != std::string::npos ||
            response.find('\007')  != std::string::npos) {
            break;
        }
    }

    // 恢复终端状态
    tcsetattr(STDIN_FILENO, TCSANOW, &old_termios);

    if (response.empty()) {
        return std::nullopt;
    }

    // 提取 "rgb:..." 部分
    auto rgb_pos = response.find("rgb:");
    if (rgb_pos == std::string::npos) {
        LOG_WARN("[theme_detect] OSC 11 response missing 'rgb:': " + response);
        return std::nullopt;
    }

    auto end = response.find('\033', rgb_pos);
    if (end == std::string::npos) {
        end = response.find('\007', rgb_pos);
    }
    if (end == std::string::npos) {
        end = response.size();
    }

    return response.substr(rgb_pos, end - rgb_pos);
}

// POSIX 没有统一的系统暗色模式 API(macOS 有 osascript 但太重)。
std::optional<bool> probe_system_dark_mode() {
    return std::nullopt;
}

} // namespace acecode

#endif // !_WIN32
