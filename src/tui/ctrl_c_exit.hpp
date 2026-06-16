#pragma once

#include <chrono>

namespace acecode::tui {

inline constexpr std::chrono::milliseconds kCtrlCExitWindow{1000};

enum class CtrlCExitAction {
    Arm,
    Exit,
};

inline void clear_ctrl_c_exit_state(
    bool& armed,
    std::chrono::steady_clock::time_point& last_press) {
    armed = false;
    last_press = {};
}

inline bool ctrl_c_exit_expired(
    bool armed,
    std::chrono::steady_clock::time_point last_press,
    std::chrono::steady_clock::time_point now,
    std::chrono::milliseconds window = kCtrlCExitWindow) {
    if (!armed) {
        return false;
    }
    return now - last_press > window;
}

inline bool expire_ctrl_c_exit_state(
    bool& armed,
    std::chrono::steady_clock::time_point& last_press,
    std::chrono::steady_clock::time_point now,
    std::chrono::milliseconds window = kCtrlCExitWindow) {
    if (!ctrl_c_exit_expired(armed, last_press, now, window)) {
        return false;
    }
    clear_ctrl_c_exit_state(armed, last_press);
    return true;
}

inline CtrlCExitAction record_ctrl_c_exit_press(
    bool& armed,
    std::chrono::steady_clock::time_point& last_press,
    std::chrono::steady_clock::time_point now,
    std::chrono::milliseconds window = kCtrlCExitWindow) {
    if (armed && !ctrl_c_exit_expired(armed, last_press, now, window)) {
        clear_ctrl_c_exit_state(armed, last_press);
        return CtrlCExitAction::Exit;
    }
    armed = true;
    last_press = now;
    return CtrlCExitAction::Arm;
}

} // namespace acecode::tui
