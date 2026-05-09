// acetui/terminal.hpp — 终端原始模式 + VT processing + 事件拉取的统一入口。
//
// Win32 端在 src/terminal_win.cpp 实现(SetConsoleMode + ReadConsoleInputW +
// WriteFile);POSIX 端在 src/terminal_posix.cpp 实现(termios + read +
// ioctl(TIOCGWINSZ))。本头文件不暴露平台细节,所有内部状态以 anonymous
// namespace 形式藏在 cpp 里。
//
// Terminal 是一个无构造的静态接口 — 进程内只可能存在一个真实终端,所以
// 用类型作命名空间而不是要求实例化。

#pragma once

#include <chrono>
#include <optional>
#include <string_view>

#include "acetui/event.hpp"

namespace acetui {

struct Size {
    int width  = 0;
    int height = 0;
};

class Terminal {
 public:
    // 进入 raw mode + enable VT processing。失败返回 false,内部不抛异常,
    // 不留下半改的 mode。已成功一次的二次调用是 no-op。
    static bool enable_raw();

    // 恢复保存的原 console mode + codepage。多次调用安全。
    static void restore();

    // 当前可视窗口尺寸(不是 buffer 总尺寸)。失败时返回最后一次成功值
    // 或保守默认 80x24。
    static Size size();

    // 写一段字节到 stdout。短写入会循环补完。
    static void write(std::string_view bytes);

    // 拉一个事件。
    //   timeout = nullopt → 阻塞,直到有事件返回。
    //   timeout = X       → 在 X 内有事件返回事件,超时返回 nullopt。
    static std::optional<Event> read_event(
        std::optional<std::chrono::milliseconds> timeout = std::nullopt);
};

}  // namespace acetui
