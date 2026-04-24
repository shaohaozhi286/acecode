#include "ask_overlay_input.hpp"

#include "../tui_state.hpp"
#include "../utils/text_input_ops.hpp"

#include <string>

namespace acecode {

namespace {

// Home 等价按键集合:FTXUI 的 Event::Home + 几个 VT 系列 / rxvt 系列的
// ESC-sequence 原始字节 + Ctrl+A。与 main.cpp 的 is_home_event 保持同步。
bool is_home_event(const ftxui::Event& e) {
    return e == ftxui::Event::Home
        || e == ftxui::Event::Special("\x1B[1~")
        || e == ftxui::Event::Special("\x1B[7~")
        || e == ftxui::Event::Special(std::string(1, '\x01')); // Ctrl+A
}

bool is_end_event(const ftxui::Event& e) {
    return e == ftxui::Event::End
        || e == ftxui::Event::Special("\x1B[4~")
        || e == ftxui::Event::Special("\x1B[8~")
        || e == ftxui::Event::Special(std::string(1, '\x05')); // Ctrl+E
}

} // namespace

bool try_handle_ask_other_input(TuiState& state, const ftxui::Event& event) {
    // 可打印字符 —— 内联处理,不触发 slash-dropdown refresh / shell-mode
    // trigger / history_index 重置等所有无关副作用。
    if (event.is_character()) {
        insert_at_cursor(state.input_text, state.input_cursor, event.character());
        return true;
    }

    if (event == ftxui::Event::Backspace) {
        backspace_utf8(state.input_text, state.input_cursor);
        return true;
    }

    if (event == ftxui::Event::Delete) {
        delete_utf8(state.input_text, state.input_cursor);
        return true;
    }

    if (event == ftxui::Event::ArrowLeft) {
        move_cursor_left_utf8(state.input_text, state.input_cursor);
        return true;
    }

    if (event == ftxui::Event::ArrowRight) {
        move_cursor_right_utf8(state.input_text, state.input_cursor);
        return true;
    }

    if (is_home_event(event)) {
        state.input_cursor = 0;
        return true;
    }

    if (is_end_event(event)) {
        state.input_cursor = state.input_text.size();
        return true;
    }

    // 未识别按键(例如 Event::Custom / Event::Mouse / F1 等):helper 不
    // 改 state,返回 false 告诉调用方 **不要** PostEvent。调用方仍应该
    // 吞掉事件(给 FTXUI 返回 true),防止未识别键透传到下游 handler,
    // 但 PostEvent 仅当 state 真的变了才触发 —— 否则 Custom 事件自回环
    // 会把事件循环卡死。
    return false;
}

} // namespace acecode
