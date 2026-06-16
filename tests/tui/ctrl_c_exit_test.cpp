#include <gtest/gtest.h>

#include "tui/ctrl_c_exit.hpp"

using acecode::tui::CtrlCExitAction;
using acecode::tui::clear_ctrl_c_exit_state;
using acecode::tui::expire_ctrl_c_exit_state;
using acecode::tui::record_ctrl_c_exit_press;

namespace {

using Clock = std::chrono::steady_clock;

Clock::time_point at_ms(int ms) {
    return Clock::time_point{} + std::chrono::milliseconds(ms);
}

} // namespace

TEST(CtrlCExit, FirstPressArmsState) {
    bool armed = false;
    Clock::time_point last_press{};

    EXPECT_EQ(record_ctrl_c_exit_press(armed, last_press, at_ms(1000)),
              CtrlCExitAction::Arm);

    EXPECT_TRUE(armed);
    EXPECT_EQ(last_press, at_ms(1000));
}

TEST(CtrlCExit, SecondPressWithinOneSecondExits) {
    bool armed = false;
    Clock::time_point last_press{};

    ASSERT_EQ(record_ctrl_c_exit_press(armed, last_press, at_ms(1000)),
              CtrlCExitAction::Arm);

    EXPECT_EQ(record_ctrl_c_exit_press(armed, last_press, at_ms(2000)),
              CtrlCExitAction::Exit);
    EXPECT_FALSE(armed);
    EXPECT_EQ(last_press, Clock::time_point{});
}

TEST(CtrlCExit, SecondPressAfterTimeoutRearms) {
    bool armed = false;
    Clock::time_point last_press{};

    ASSERT_EQ(record_ctrl_c_exit_press(armed, last_press, at_ms(1000)),
              CtrlCExitAction::Arm);

    EXPECT_EQ(record_ctrl_c_exit_press(armed, last_press, at_ms(2001)),
              CtrlCExitAction::Arm);
    EXPECT_TRUE(armed);
    EXPECT_EQ(last_press, at_ms(2001));
}

TEST(CtrlCExit, TimeoutExpiryClearsState) {
    bool armed = false;
    Clock::time_point last_press{};

    ASSERT_EQ(record_ctrl_c_exit_press(armed, last_press, at_ms(1000)),
              CtrlCExitAction::Arm);

    EXPECT_FALSE(expire_ctrl_c_exit_state(armed, last_press, at_ms(2000)));
    EXPECT_TRUE(armed);

    EXPECT_TRUE(expire_ctrl_c_exit_state(armed, last_press, at_ms(2001)));
    EXPECT_FALSE(armed);
    EXPECT_EQ(last_press, Clock::time_point{});
}

TEST(CtrlCExit, NonCtrlCEditCancellationClearsState) {
    bool armed = false;
    Clock::time_point last_press{};

    ASSERT_EQ(record_ctrl_c_exit_press(armed, last_press, at_ms(1000)),
              CtrlCExitAction::Arm);

    clear_ctrl_c_exit_state(armed, last_press);

    EXPECT_EQ(record_ctrl_c_exit_press(armed, last_press, at_ms(1500)),
              CtrlCExitAction::Arm);
    EXPECT_TRUE(armed);
    EXPECT_EQ(last_press, at_ms(1500));
}
