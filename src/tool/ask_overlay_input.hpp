#pragma once

// AskUserQuestion overlay 的 "Other..." 自定义文本输入态事件分发 helper。
//
// 背景:`main.cpp` 的 CatchEvent lambda 把所有输入事件都塞在一个巨大闭包里。
// Other 输入态原本走的是 "return false 让事件冒泡到下游输入框" 思路,但是
// FTXUI 的 input_renderer 实际上是个 Renderer(纯渲染),不是 Input 组件 ——
// 返回 false 的事件会被直接丢掉,用户完全无法键入。(参见
// openspec/changes/fix-ask-user-question-other-input)
//
// 本 helper 把 Other 输入态的所有按键拦截集中到一处纯数据变换函数里,
// main.cpp 调用它 → 永远 return true(消耗事件),绕开下游 shell-mode trigger /
// slash-dropdown refresh / Ctrl+E tool_result-expand / history_index 重置 /
// IME 等所有不该在 Other 态触发的副作用。
//
// 锁契约:调用方 MUST 在已持有 `TuiState::mu` 的情况下调本函数。helper
// 自身不做锁,也不 PostEvent —— PostEvent 由调用方在返回 true 时触发。
//
// 返回值语义:返回 **"是否修改了 state"** —— 仅当本次调用真的写了
// input_text / input_cursor / 光标位置时返回 true,调用方据此触发一次
// PostEvent(Custom) 让 TUI 重绘。对于未识别的按键(例如 Event::Custom /
// Event::Mouse / F1 等),helper 返回 false,调用方 MUST NOT PostEvent
// (否则会形成 "Custom → swallow → PostEvent(Custom) → ..." 的无限事件回环,
// 把事件循环卡死)。注意:无论返回 true / false,调用方都应该把事件消耗
// 掉(即 return true 给 FTXUI),防止未识别键透传到下游 handler(如
// shell-mode trigger / slash-dropdown 等)。

#include <ftxui/component/event.hpp>

namespace acecode {

struct TuiState;

// 在 Other 输入态下尝试消费一个按键事件。返回 true 表示已经改了 state
// (调用方应该 PostEvent 请求重绘);返回 false 表示按键未被识别、state
// 未变(调用方仍应吞掉事件,但 MUST NOT PostEvent)。
//
// 识别的按键:
//   可打印字符 → 在 input_cursor 位置插入到 input_text,推进 cursor
//   Backspace  → 按 UTF-8 glyph 回退删除
//   Delete     → 按 UTF-8 glyph 向右删除
//   ArrowLeft / ArrowRight → 按 UTF-8 glyph 移动 cursor
//   Home / End(含 ESC-sequence 变体 `ESC [1~` `[7~` `[4~` `[8~`)及
//   Ctrl+A / Ctrl+E → cursor 跳到 buffer 首 / 尾
//
// 调用方需要保证 state.ask_pending && state.ask_other_input_active;
// Return / Escape 留给 main.cpp 的 ask 分支上层处理(它们有 submit / 退出
// Other 态的特殊语义),MUST NOT 传入本函数。
bool try_handle_ask_other_input(TuiState& state, const ftxui::Event& event);

} // namespace acecode
