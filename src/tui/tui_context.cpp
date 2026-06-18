#include "tui/tui_context.hpp"

#include <chrono>
#include <mutex>

#include <ftxui/component/event.hpp>

#include "tool/mcp_startup_coordination.hpp"

namespace acecode {

void TuiContext::coordinate_mcp_before_first_turn() {
    auto result = acecode::coordinate_mcp_before_first_turn(
        mcp_manager,
        mcp_first_turn_wait_done,
        std::chrono::milliseconds(1500));
    if (result.should_warn) {
        std::lock_guard<std::mutex> lk(state.mu);
        state.conversation.push_back({
            "system",
            acecode::mcp_first_turn_still_starting_warning(),
            false,
        });
        screen.PostEvent(ftxui::Event::Custom);
    }
}

} // namespace acecode
