// 覆盖 src/tool/ask_overlay_input.cpp 的 try_handle_ask_other_input 分发:
//   AskUserQuestion "Other..." 自由文本输入态下,键盘事件被这里的 helper
//   内联消耗。本测试验证:
//     - 字符事件 → 按 UTF-8 字节插入到 TuiState::input_text,cursor 推进;
//     - Backspace / Delete / ArrowLeft / ArrowRight 按 glyph 生效;
//     - Home / End 及其 ESC-sequence 和 Ctrl+A/E 等价键把 cursor 跳到端点;
//     - MUST NOT 产生副作用:首字符 '!' 不误触 Shell 模式、'/' 不弹
//       slash-dropdown、`history_index` 保持 -1、未识别按键被吞掉;
//     - helper 永远返回 true(事件被消耗),调用方照样 return 这个值。
// 这组测试是为了对齐 openspec/changes/fix-ask-user-question-other-input 的
// 回归契约(Scenario: Other 态下字符键写入 input_text 等一系列 MUST)。

#include <gtest/gtest.h>

#include "tool/ask_overlay_input.hpp"
#include "tui_state.hpp"

#include <ftxui/component/event.hpp>

#include <cstddef>
#include <string>

using acecode::try_handle_ask_other_input;
using acecode::TuiState;
using acecode::InputMode;
using ftxui::Event;

namespace {

// 初始化一个典型 "Other 输入态" 的 TuiState:ask_pending=true、
// ask_other_input_active=true,关心的副作用源字段(input_text /
// input_cursor / input_mode / slash_dropdown_active / history_index)
// 被显式归零。用 by-reference 是因为 TuiState 含 std::mutex /
// std::condition_variable,不能按值返回 / 拷贝。
void init_other_mode_state(TuiState& s) {
    s.ask_pending = true;
    s.ask_other_input_active = true;
    s.input_text.clear();
    s.input_cursor = 0;
    s.input_mode = InputMode::Normal;
    s.history_index = -1;
    s.slash_dropdown_active = false;
}

} // namespace

// 场景:连按两个字符 'h' 'i' → input_text=="hi" / cursor==2,helper 返回 true。
TEST(AskOverlayInputTest, CharacterInsertionAccumulates) {
    TuiState s;
    init_other_mode_state(s);
    EXPECT_TRUE(try_handle_ask_other_input(s, Event::Character("h")));
    EXPECT_EQ(s.input_text, "h");
    EXPECT_EQ(s.input_cursor, 1u);
    EXPECT_TRUE(try_handle_ask_other_input(s, Event::Character("i")));
    EXPECT_EQ(s.input_text, "hi");
    EXPECT_EQ(s.input_cursor, 2u);
}

// 场景:input_text="中文"(6 字节)/ cursor=6 按 Backspace → "中" / cursor=3
//       (一次退格吞掉整个 UTF-8 多字节 glyph)。
TEST(AskOverlayInputTest, BackspaceDeletesFullUtf8Glyph) {
    TuiState s;
    init_other_mode_state(s);
    s.input_text = "\xE4\xB8\xAD\xE6\x96\x87"; // "中文" (6 bytes)
    s.input_cursor = 6;
    EXPECT_TRUE(try_handle_ask_other_input(s, Event::Backspace));
    EXPECT_EQ(s.input_text, "\xE4\xB8\xAD"); // "中"
    EXPECT_EQ(s.input_cursor, 3u);
}

// 场景:"abc" / cursor=3,按 ArrowLeft 两次 → cursor==1(逐字节后退单字节字符)。
TEST(AskOverlayInputTest, ArrowLeftMovesCursorByGlyph) {
    TuiState s;
    init_other_mode_state(s);
    s.input_text = "abc";
    s.input_cursor = 3;
    EXPECT_TRUE(try_handle_ask_other_input(s, Event::ArrowLeft));
    EXPECT_EQ(s.input_cursor, 2u);
    EXPECT_TRUE(try_handle_ask_other_input(s, Event::ArrowLeft));
    EXPECT_EQ(s.input_cursor, 1u);
}

// 场景:"hello" / cursor=2,按 Home → cursor=0;再按 End → cursor=5。
TEST(AskOverlayInputTest, HomeAndEndJumpToEdges) {
    TuiState s;
    init_other_mode_state(s);
    s.input_text = "hello";
    s.input_cursor = 2;
    EXPECT_TRUE(try_handle_ask_other_input(s, Event::Home));
    EXPECT_EQ(s.input_cursor, 0u);
    EXPECT_TRUE(try_handle_ask_other_input(s, Event::End));
    EXPECT_EQ(s.input_cursor, 5u);
}

// 场景:VT220 风格 Home(`ESC [ 1 ~`)也应触发 cursor=0,验证 ESC-sequence
// 等价集合命中(非所有终端都发 Event::Home)。
TEST(AskOverlayInputTest, Vt220HomeEscapeAlsoWorks) {
    TuiState s;
    init_other_mode_state(s);
    s.input_text = "hello";
    s.input_cursor = 3;
    EXPECT_TRUE(try_handle_ask_other_input(s, Event::Special("\x1B[1~")));
    EXPECT_EQ(s.input_cursor, 0u);
}

// 场景:Ctrl+A(readline-style)等价于 Home → cursor=0。
TEST(AskOverlayInputTest, CtrlAEquivalentToHome) {
    TuiState s;
    init_other_mode_state(s);
    s.input_text = "hello";
    s.input_cursor = 4;
    EXPECT_TRUE(try_handle_ask_other_input(s, Event::Special(std::string(1, '\x01'))));
    EXPECT_EQ(s.input_cursor, 0u);
}

// 场景:Ctrl+E(readline-style)等价于 End → cursor=text.size();
// OQ2 决定 Other 态下 Ctrl+E 不走 tool_result-expand 分支,因为 helper
// 内部完全没有 chat_focus_index 逻辑。
TEST(AskOverlayInputTest, CtrlEEquivalentToEnd) {
    TuiState s;
    init_other_mode_state(s);
    s.input_text = "hello";
    s.input_cursor = 1;
    EXPECT_TRUE(try_handle_ask_other_input(s, Event::Special(std::string(1, '\x05'))));
    EXPECT_EQ(s.input_cursor, 5u);
}

// 场景:input_mode=Normal / input_text="" / 按 '!' → 仅写入 "!",
// input_mode MUST 保持 Normal(不能误触发 main.cpp 的 Shell-mode trigger)。
TEST(AskOverlayInputTest, ExclamationDoesNotToggleShellMode) {
    TuiState s;
    init_other_mode_state(s);
    EXPECT_TRUE(try_handle_ask_other_input(s, Event::Character("!")));
    EXPECT_EQ(s.input_text, "!");
    EXPECT_EQ(s.input_cursor, 1u);
    EXPECT_EQ(s.input_mode, InputMode::Normal);
}

// 场景:input_text="" / 按 '/' → 写入 "/",slash_dropdown_active MUST
// 保持 false(helper 不调 refresh_slash_dropdown)。
TEST(AskOverlayInputTest, SlashDoesNotOpenDropdown) {
    TuiState s;
    init_other_mode_state(s);
    EXPECT_TRUE(try_handle_ask_other_input(s, Event::Character("/")));
    EXPECT_EQ(s.input_text, "/");
    EXPECT_FALSE(s.slash_dropdown_active);
}

// 场景:字符事件 MUST NOT 重置 history_index。Helper 对 history_index
// 完全不关心 —— 下层 text_input_ops 纯函数甚至看不到这个字段。
TEST(AskOverlayInputTest, HistoryIndexNotResetByCharacter) {
    TuiState s;
    init_other_mode_state(s);
    s.history_index = 5; // 模拟用户之前翻到历史第 5 条
    EXPECT_TRUE(try_handle_ask_other_input(s, Event::Character("a")));
    EXPECT_EQ(s.history_index, 5);
}

// 场景:未识别按键(例如 F1 / Custom)→ helper **返回 false**(state 没变,
// 调用方 MUST NOT 为此 PostEvent,否则 Custom 事件会自回环);
// input_text / input_cursor 必须保持不变。
// 这条回归是为了防止 "Custom → swallow → PostEvent(Custom) → ..." 事件回环
// 把 TUI 打爆(实机验证过会直接卡死)。
TEST(AskOverlayInputTest, UnrecognizedEventReturnsFalseWithoutStateChange) {
    TuiState s;
    init_other_mode_state(s);
    s.input_text = "partial";
    s.input_cursor = 4;
    EXPECT_FALSE(try_handle_ask_other_input(s, Event::F1));
    EXPECT_EQ(s.input_text, "partial");
    EXPECT_EQ(s.input_cursor, 4u);

    // Event::Custom 也必须返回 false —— 这是实机卡死的直接原因。
    EXPECT_FALSE(try_handle_ask_other_input(s, Event::Custom));
    EXPECT_EQ(s.input_text, "partial");
    EXPECT_EQ(s.input_cursor, 4u);
}
