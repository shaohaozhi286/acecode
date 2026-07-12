#pragma once

#include "tui/chat_scroll.hpp"

#include <algorithm>
#include <vector>

namespace acecode::tui {

struct ChatRenderWindow {
    int first_message = 0;
    int last_message_exclusive = 0;
    int top_spacer_rows = 0;
    int bottom_spacer_rows = 0;
    int overscan_rows = 0;
    int total_rows = 0;
};

inline int default_chat_render_overscan_rows(int viewport_rows) {
    if (viewport_rows <= 0) {
        return 0;
    }
    return std::max(24, viewport_rows * 2);
}

inline ChatRenderWindow full_chat_render_window(
    const std::vector<int>& line_counts,
    int message_count,
    const std::vector<int>& spacer_rows_after) {
    ChatRenderWindow out;
    out.first_message = 0;
    out.last_message_exclusive = std::max(0, message_count);
    out.total_rows = chat_transcript_display_rows(
        line_counts, out.last_message_exclusive, spacer_rows_after);
    return out;
}

inline ChatRenderWindow chat_render_window(
    const std::vector<int>& line_counts,
    int message_count,
    int scroll_top_row,
    int viewport_rows,
    int overscan_rows,
    const std::vector<int>& spacer_rows_after) {
    message_count = std::max(0, message_count);
    ChatRenderWindow out;
    out.overscan_rows = std::max(0, overscan_rows);
    out.total_rows = chat_transcript_display_rows(
        line_counts, message_count, spacer_rows_after);

    if (message_count <= 0 || out.total_rows <= 0) {
        return out;
    }

    if (viewport_rows <= 0) {
        out = full_chat_render_window(
            line_counts, message_count, spacer_rows_after);
        out.overscan_rows = std::max(0, overscan_rows);
        return out;
    }

    const int clamped_top = clamp_chat_scroll_top_row(
        scroll_top_row, line_counts, message_count, viewport_rows,
        spacer_rows_after);
    const int window_start = std::max(0, clamped_top - out.overscan_rows);
    const int window_end = std::min(
        out.total_rows, clamped_top + viewport_rows + out.overscan_rows);

    int row = 0;
    int first = -1;
    int first_start = 0;
    int last = -1;
    int after_last = 0;

    for (int i = 0; i < message_count; ++i) {
        const int message_start = row;
        const int message_rows = chat_line_count_at(line_counts, i) +
            chat_spacer_rows_after_at(spacer_rows_after, i);
        const int message_end = message_start + message_rows;
        if (message_end > window_start && message_start < window_end) {
            if (first < 0) {
                first = i;
                first_start = message_start;
            }
            last = i + 1;
            after_last = message_end;
        }
        row = message_end;
    }

    if (first < 0) {
        first = message_count - 1;
        const int fallback_rows = chat_line_count_at(line_counts, first) +
            chat_spacer_rows_after_at(spacer_rows_after, first);
        first_start = std::max(0, out.total_rows - fallback_rows);
        last = message_count;
        after_last = out.total_rows;
    }

    out.first_message = first;
    out.last_message_exclusive = last;
    out.top_spacer_rows = first_start;
    out.bottom_spacer_rows = std::max(0, out.total_rows - after_last);
    return out;
}

} // namespace acecode::tui
