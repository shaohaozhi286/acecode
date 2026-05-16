#pragma once

#include <algorithm>
#include <vector>

namespace acecode::tui {

inline int chat_line_count_at(const std::vector<int>& line_counts, int index) {
    if (index < 0 || index >= static_cast<int>(line_counts.size())) {
        return 1;
    }
    return line_counts[index] > 0 ? line_counts[index] : 1;
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

} // namespace acecode::tui
