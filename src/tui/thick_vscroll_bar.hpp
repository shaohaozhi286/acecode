#pragma once

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include <utility>
#include <vector>

namespace acecode::tui {

// Vertical scrollbar decorator that reserves `width` columns at the right edge
// of `child` and renders a rail + thumb in those columns. Functionally a
// drop-in replacement for ftxui::vscroll_indicator with two differences:
//   1. width is configurable (default 2 at the call site).
//   2. The decorator publishes its own track Box via out_track_box every
//      frame so the host can hit-test mouse events that fall on the
//      scrollbar columns.
//
// Lifetime: out_track_box must outlive the returned Element. The decorator
// stores a raw pointer; the caller owns the storage (typically a stack
// `Box scrollbar_box;` that lives next to the Renderer lambda).
//
// When the content fits inside the visible track (no scrolling possible),
// the rail/thumb glyphs are NOT drawn — the reserved columns render as
// whitespace — but the track Box is still populated so callers don't have to
// branch on visibility for hit-testing (the y_to_focus mapping handles the
// no-scroll case by returning {0,0}).
ftxui::Element thick_vscroll_bar(ftxui::Element child,
                                 int width,
                                 ftxui::Box& out_track_box);

// Pure mapping from mouse y-coordinate to (focus_index, line_offset) for
// a chat-style message list. No FTXUI dependency — exposed inline so unit
// tests can link against the testable library without pulling in src/tui/.
//
// Inputs:
//   mouse_y               — terminal Y coordinate of the click/drag cursor
//   track_y_min           — first Y coordinate of the scrollbar track
//   track_height          — number of rows the track spans (>= 1)
//   message_line_counts   — height (in rows) of each message in render order
//
// Returns {focus_index, line_offset}:
//   - focus_index ∈ [0, counts.size()-1]: which message the cursor maps to
//   - line_offset ∈ [0, lines_in_that_msg-1]: which line within the message
//
// Edge cases:
//   - empty counts             → {-1, 0}, caller should no-op
//   - sum(counts) ≤ track_h    → {0, 0}, no scroll possible
//   - mouse_y < track_y_min    → clamped to first line of first message
//   - mouse_y > track_y_max    → clamped to last line of last message
//   - counts[i] == 0           → treated as 1 (mirrors lines_of helper in
//                                main.cpp scroll_chat_by_lines)
inline std::pair<int, int> y_to_focus(
    int mouse_y,
    int track_y_min,
    int track_height,
    const std::vector<int>& message_line_counts) {
    if (message_line_counts.empty()) {
        return {-1, 0};
    }

    long long total = 0;
    for (int c : message_line_counts) {
        total += (c > 0 ? c : 1);
    }
    if (total <= track_height) {
        return {0, 0};
    }
    if (track_height <= 1) {
        // Degenerate track — there is no spatial dimension to map across.
        return {0, 0};
    }

    int rel = mouse_y - track_y_min;
    if (rel < 0) rel = 0;
    if (rel > track_height - 1) rel = track_height - 1;

    long long target =
        static_cast<long long>(rel) * (total - 1) / (track_height - 1);
    if (target < 0) target = 0;
    if (target > total - 1) target = total - 1;

    long long cumulative = 0;
    for (size_t i = 0; i < message_line_counts.size(); ++i) {
        int cur = message_line_counts[i] > 0 ? message_line_counts[i] : 1;
        if (target < cumulative + cur) {
            return {static_cast<int>(i),
                    static_cast<int>(target - cumulative)};
        }
        cumulative += cur;
    }

    int last = static_cast<int>(message_line_counts.size()) - 1;
    int last_lines =
        message_line_counts[last] > 0 ? message_line_counts[last] : 1;
    return {last, last_lines - 1};
}

} // namespace acecode::tui
