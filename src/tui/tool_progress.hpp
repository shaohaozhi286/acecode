#pragma once

#include "../tui_state.hpp"

#include <ftxui/dom/elements.hpp>

namespace acecode {

// Render the live tool-progress element shown between message_view and
// prompt_line while state.tool_running is true. Returns text("") when not
// running. Caller holds state.mu.
//
// 底部状态条曾有两个冗余计时 chip(render_tool_timer_chip /
// render_thinking_timer_chip),inline-thinking-heartbeat change 删除:
// 本元素与 thinking_element 都在固定布局区,"被 overlay 遮挡/滚出视野"
// 的前提不成立;等待期的耗时+token 心跳移到了推理指示行内联段
// (tui/thinking_heartbeat.hpp)。
ftxui::Element render_tool_progress(const TuiState& state);

} // namespace acecode
