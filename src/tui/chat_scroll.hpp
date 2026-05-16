#pragma once

#include <algorithm>
#include <utility>
#include <vector>

namespace acecode::tui {

inline int chat_line_count_at(const std::vector<int>& line_counts, int index) {
    if (index < 0 || index >= static_cast<int>(line_counts.size())) {
        return 1;
    }
    return line_counts[index] > 0 ? line_counts[index] : 1;
}

inline int update_chat_line_count_estimate(int previous, int measured) {
    if (measured <= 0) {
        return previous > 0 ? previous : 1;
    }
    if (previous <= 0) {
        return measured;
    }
    return std::max(previous, measured);
}

inline int clamp_chat_line_offset(int offset, int line_count) {
    const int rows = line_count > 0 ? line_count : 1;
    return std::clamp(offset, 0, rows - 1);
}

inline int chat_tail_line_offset(const std::vector<int>& line_counts,
                                 int message_index) {
    return chat_line_count_at(line_counts, message_index) - 1;
}

inline bool is_chat_tail_position(int focus_index,
                                  int line_offset,
                                  int message_count,
                                  const std::vector<int>& line_counts) {
    if (message_count <= 0 || focus_index != message_count - 1) {
        return false;
    }
    return line_offset >= chat_tail_line_offset(line_counts, focus_index);
}

inline int chat_transcript_display_rows(const std::vector<int>& line_counts,
                                        int message_count) {
    if (message_count <= 0) {
        return 0;
    }

    int rows = 0;
    for (int i = 0; i < message_count; ++i) {
        rows += chat_line_count_at(line_counts, i);
        rows += 1; // Spacer row rendered after every message.
    }
    return rows;
}

inline int chat_max_scroll_top_row(const std::vector<int>& line_counts,
                                   int message_count,
                                   int viewport_rows) {
    if (message_count <= 0 || viewport_rows <= 0) {
        return 0;
    }

    const int transcript_rows =
        chat_transcript_display_rows(line_counts, message_count);
    return std::max(0, transcript_rows - viewport_rows);
}

inline int clamp_chat_scroll_top_row(int scroll_top_row,
                                     const std::vector<int>& line_counts,
                                     int message_count,
                                     int viewport_rows) {
    return std::clamp(scroll_top_row, 0,
                      chat_max_scroll_top_row(line_counts, message_count,
                                              viewport_rows));
}

inline int chat_display_row_for_focus(const std::vector<int>& line_counts,
                                      int message_count,
                                      int focus_index,
                                      int line_offset) {
    if (message_count <= 0) {
        return 0;
    }

    focus_index = std::clamp(focus_index, 0, message_count - 1);
    int row = 0;
    for (int i = 0; i < focus_index; ++i) {
        row += chat_line_count_at(line_counts, i);
        row += 1; // Spacer row rendered after every message.
    }
    row += clamp_chat_line_offset(line_offset,
                                  chat_line_count_at(line_counts,
                                                     focus_index));
    return row;
}

inline std::pair<int, int> chat_focus_from_display_row(
    const std::vector<int>& line_counts,
    int message_count,
    int display_row) {
    if (message_count <= 0) {
        return {-1, 0};
    }

    const int transcript_rows =
        chat_transcript_display_rows(line_counts, message_count);
    if (transcript_rows <= 0) {
        return {-1, 0};
    }
    display_row = std::clamp(display_row, 0, transcript_rows - 1);

    int row = 0;
    for (int i = 0; i < message_count; ++i) {
        const int lines = chat_line_count_at(line_counts, i);
        if (display_row < row + lines) {
            return {i, display_row - row};
        }
        row += lines;

        // Spacer rows are not real message content. Map them to the previous
        // message's tail so commands that act on the focused message remain
        // stable while the viewport top crosses spacing.
        if (display_row == row) {
            return {i, lines - 1};
        }
        row += 1;
    }

    const int last = message_count - 1;
    return {last, chat_tail_line_offset(line_counts, last)};
}

inline int chat_frame_focus_y_for_scroll_top(int scroll_top_row,
                                             int viewport_rows) {
    const int top = std::max(0, scroll_top_row);
    const int external_dim = std::max(0, viewport_rows - 1);
    return top + external_dim / 2;
}

struct ChatScrollbarThumbGeometry {
    int max_top_row = 0;
    int scroll_range_2x = 0;
    int thumb_size_2x = 0;
    int thumb_top_2x = 0;
};

inline ChatScrollbarThumbGeometry chat_scrollbar_thumb_geometry(
    int track_y_min,
    int track_height,
    const std::vector<int>& line_counts,
    int message_count,
    int viewport_rows,
    int scroll_top_row) {
    ChatScrollbarThumbGeometry out;
    out.thumb_top_2x = 2 * track_y_min;

    const int track_2x = std::max(0, 2 * track_height);
    if (track_2x <= 0) {
        return out;
    }

    const int content_rows =
        chat_transcript_display_rows(line_counts, message_count);
    out.max_top_row =
        chat_max_scroll_top_row(line_counts, message_count, viewport_rows);
    if (content_rows <= 0 || out.max_top_row <= 0) {
        out.thumb_size_2x = track_2x;
        return out;
    }

    const int min_thumb_2x = std::min(6, track_2x);
    int thumb_size = static_cast<int>(
        static_cast<long long>(2 * track_height) * track_height /
        content_rows);
    thumb_size = std::max(thumb_size, min_thumb_2x);
    thumb_size = std::min(thumb_size, track_2x);
    out.thumb_size_2x = thumb_size;
    out.scroll_range_2x = track_2x - thumb_size;

    const int clamped_top = clamp_chat_scroll_top_row(
        scroll_top_row, line_counts, message_count, viewport_rows);
    if (out.scroll_range_2x > 0) {
        out.thumb_top_2x += static_cast<int>(
            static_cast<long long>(out.scroll_range_2x) * clamped_top /
            out.max_top_row);
    }
    return out;
}

inline int chat_scrollbar_grab_offset_2x(
    int mouse_y,
    const ChatScrollbarThumbGeometry& geometry) {
    if (geometry.scroll_range_2x <= 0 || geometry.thumb_size_2x <= 0) {
        return 0;
    }

    const int mouse_2x = 2 * mouse_y;
    if (mouse_2x >= geometry.thumb_top_2x &&
        mouse_2x <= geometry.thumb_top_2x + geometry.thumb_size_2x) {
        return std::clamp(mouse_2x - geometry.thumb_top_2x,
                          0, geometry.thumb_size_2x);
    }

    // Clicking the track outside the thumb centers the thumb on the pointer.
    return geometry.thumb_size_2x / 2;
}

inline int chat_scrollbar_y_to_top_row_with_grab(
    int mouse_y,
    int track_y_min,
    const ChatScrollbarThumbGeometry& geometry,
    int grab_offset_2x) {
    if (geometry.max_top_row <= 0 || geometry.scroll_range_2x <= 0) {
        return 0;
    }

    int thumb_top_rel_2x =
        2 * mouse_y - grab_offset_2x - 2 * track_y_min;
    thumb_top_rel_2x =
        std::clamp(thumb_top_rel_2x, 0, geometry.scroll_range_2x);

    return static_cast<int>(
        static_cast<long long>(thumb_top_rel_2x) * geometry.max_top_row /
        geometry.scroll_range_2x);
}

inline int chat_scrollbar_y_to_top_row(
    int mouse_y,
    int track_y_min,
    int track_height,
    const std::vector<int>& line_counts,
    int message_count,
    int viewport_rows) {
    const int max_top =
        chat_max_scroll_top_row(line_counts, message_count, viewport_rows);
    if (max_top <= 0 || track_height <= 1) {
        return 0;
    }

    int rel = mouse_y - track_y_min;
    rel = std::clamp(rel, 0, track_height - 1);
    return static_cast<int>(
        static_cast<long long>(rel) * max_top / (track_height - 1));
}

inline int chat_bottom_anchor_top_padding_rows(
    const std::vector<int>& line_counts,
    int message_count,
    int viewport_rows) {
    if (message_count <= 0 || viewport_rows <= 0) {
        return 0;
    }

    const int transcript_rows =
        chat_transcript_display_rows(line_counts, message_count);
    if (transcript_rows >= viewport_rows) {
        return 0;
    }
    return viewport_rows - transcript_rows;
}

inline bool is_chat_mouse_target(int mouse_x,
                                 int mouse_y,
                                 int chat_x_min,
                                 int chat_y_min,
                                 int chat_x_max,
                                 int chat_y_max,
                                 bool is_wheel_event) {
    if (chat_x_min > chat_x_max || chat_y_min > chat_y_max) {
        return false;
    }
    if (mouse_x < chat_x_min || mouse_x > chat_x_max) {
        return false;
    }
    if (mouse_y >= chat_y_min && mouse_y <= chat_y_max) {
        return true;
    }

    // FTXUI TerminalOutput mode can briefly subtract a stale frame origin when
    // reusing the same terminal. Wheel events then arrive above the chat box
    // even though the pointer is over the chat transcript.
    return is_wheel_event && mouse_y < chat_y_min;
}

} // namespace acecode::tui
