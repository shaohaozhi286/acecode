#pragma once

#include <algorithm>
#include <vector>

namespace acecode::tui {

struct ChatViewportState {
    int scroll_top_row = 0;
    int target_scroll_top_row = 0;
    int viewport_rows = 0;
    int total_rows = 0;
    bool follow_tail = true;
};

struct ChatMessageLayout {
    int message_index = -1;
    int row_count = 1;
    int start_row = 0;
    int end_row = 0;
    int spacer_row = 0;
};

struct ChatDisplayRow {
    int absolute_row = 0;
    int message_index = -1;
    int message_row = 0;
    bool spacer_after_message = false;
};

struct ChatRowRange {
    int message_index = -1;
    int row_begin = 0;
    int row_end = 0;
    bool spacer_after_visible = false;

    bool has_content_rows() const {
        return row_end > row_begin;
    }
};

struct ChatVisibleRows {
    int row_begin = 0;
    int row_end = 0;
    int viewport_rows = 0;
    std::vector<ChatRowRange> messages;

    bool empty() const {
        return row_end <= row_begin || messages.empty();
    }
};

inline int chat_viewport_message_row_count(
    const std::vector<int>& message_row_counts,
    int message_index) {
    if (message_index < 0 ||
        message_index >= static_cast<int>(message_row_counts.size())) {
        return 1;
    }
    return std::max(1, message_row_counts[message_index]);
}

inline int chat_viewport_total_rows(
    const std::vector<int>& message_row_counts,
    int message_count) {
    if (message_count <= 0) {
        return 0;
    }

    int total = 0;
    for (int i = 0; i < message_count; ++i) {
        total += chat_viewport_message_row_count(message_row_counts, i);
        total += 1;
    }
    return total;
}

inline int chat_viewport_max_scroll_top_row(int total_rows,
                                            int viewport_rows) {
    if (total_rows <= 0 || viewport_rows <= 0) {
        return 0;
    }
    return std::max(0, total_rows - viewport_rows);
}

inline int chat_viewport_clamp_scroll_top_row(int scroll_top_row,
                                              int total_rows,
                                              int viewport_rows) {
    return std::clamp(
        scroll_top_row, 0,
        chat_viewport_max_scroll_top_row(total_rows, viewport_rows));
}

inline bool chat_viewport_is_at_tail(const ChatViewportState& state) {
    return state.scroll_top_row >=
        chat_viewport_max_scroll_top_row(state.total_rows,
                                         state.viewport_rows);
}

inline void chat_viewport_set_metrics(ChatViewportState& state,
                                      int total_rows,
                                      int viewport_rows) {
    state.total_rows = std::max(0, total_rows);
    state.viewport_rows = std::max(0, viewport_rows);
    if (state.follow_tail) {
        state.scroll_top_row = chat_viewport_max_scroll_top_row(
            state.total_rows, state.viewport_rows);
    } else {
        state.scroll_top_row = chat_viewport_clamp_scroll_top_row(
            state.scroll_top_row, state.total_rows, state.viewport_rows);
        state.follow_tail = chat_viewport_is_at_tail(state);
    }
    state.target_scroll_top_row = state.scroll_top_row;
}

inline std::vector<ChatMessageLayout> chat_viewport_build_message_layouts(
    const std::vector<int>& message_row_counts,
    int message_count) {
    std::vector<ChatMessageLayout> layouts;
    if (message_count <= 0) {
        return layouts;
    }

    layouts.reserve(static_cast<size_t>(message_count));
    int cursor = 0;
    for (int i = 0; i < message_count; ++i) {
        const int row_count =
            chat_viewport_message_row_count(message_row_counts, i);
        ChatMessageLayout layout;
        layout.message_index = i;
        layout.row_count = row_count;
        layout.start_row = cursor;
        layout.end_row = cursor + row_count;
        layout.spacer_row = layout.end_row;
        layouts.push_back(layout);
        cursor = layout.end_row + 1;
    }
    return layouts;
}

inline int chat_viewport_row_for_message_line(
    const std::vector<int>& message_row_counts,
    int message_count,
    int message_index,
    int message_row) {
    if (message_count <= 0) {
        return 0;
    }

    message_index = std::clamp(message_index, 0, message_count - 1);
    int row = 0;
    for (int i = 0; i < message_index; ++i) {
        row += chat_viewport_message_row_count(message_row_counts, i);
        row += 1;
    }
    const int rows = chat_viewport_message_row_count(message_row_counts,
                                                     message_index);
    return row + std::clamp(message_row, 0, rows - 1);
}

inline ChatDisplayRow chat_viewport_display_row_at(
    const std::vector<int>& message_row_counts,
    int message_count,
    int absolute_row) {
    ChatDisplayRow out;
    if (message_count <= 0) {
        return out;
    }

    const int total =
        chat_viewport_total_rows(message_row_counts, message_count);
    absolute_row = std::clamp(absolute_row, 0, std::max(0, total - 1));
    out.absolute_row = absolute_row;

    int cursor = 0;
    for (int i = 0; i < message_count; ++i) {
        const int rows = chat_viewport_message_row_count(message_row_counts, i);
        if (absolute_row < cursor + rows) {
            out.message_index = i;
            out.message_row = absolute_row - cursor;
            out.spacer_after_message = false;
            return out;
        }
        cursor += rows;
        if (absolute_row == cursor) {
            out.message_index = i;
            out.message_row = rows - 1;
            out.spacer_after_message = true;
            return out;
        }
        cursor += 1;
    }

    const int last = message_count - 1;
    out.message_index = last;
    out.message_row =
        chat_viewport_message_row_count(message_row_counts, last) - 1;
    out.spacer_after_message = true;
    return out;
}

inline int chat_viewport_scroll_to_row(ChatViewportState& state,
                                       int row) {
    const int before = chat_viewport_clamp_scroll_top_row(
        state.scroll_top_row, state.total_rows, state.viewport_rows);
    const int after = chat_viewport_clamp_scroll_top_row(
        row, state.total_rows, state.viewport_rows);
    state.scroll_top_row = after;
    state.target_scroll_top_row = after;
    state.follow_tail = chat_viewport_is_at_tail(state);
    return after - before;
}

inline int chat_viewport_scroll_by_rows(ChatViewportState& state,
                                        int delta_rows) {
    return chat_viewport_scroll_to_row(state,
                                       state.scroll_top_row + delta_rows);
}

inline int chat_viewport_scroll_to_tail(ChatViewportState& state) {
    const int before = chat_viewport_clamp_scroll_top_row(
        state.scroll_top_row, state.total_rows, state.viewport_rows);
    const int after = chat_viewport_max_scroll_top_row(state.total_rows,
                                                       state.viewport_rows);
    state.scroll_top_row = after;
    state.target_scroll_top_row = after;
    state.follow_tail = true;
    return after - before;
}

inline int chat_viewport_ensure_row_visible(ChatViewportState& state,
                                            int absolute_row) {
    if (state.total_rows <= 0) {
        return chat_viewport_scroll_to_row(state, 0);
    }

    const int row = std::clamp(absolute_row, 0, state.total_rows - 1);
    const int top = chat_viewport_clamp_scroll_top_row(
        state.scroll_top_row, state.total_rows, state.viewport_rows);
    const int visible_rows = std::max(1, state.viewport_rows);
    if (row < top) {
        return chat_viewport_scroll_to_row(state, row);
    }
    if (row >= top + visible_rows) {
        return chat_viewport_scroll_to_row(state, row - visible_rows + 1);
    }

    state.scroll_top_row = top;
    state.target_scroll_top_row = top;
    state.follow_tail = chat_viewport_is_at_tail(state);
    return 0;
}

inline ChatVisibleRows chat_viewport_visible_rows(
    const std::vector<int>& message_row_counts,
    int message_count,
    int scroll_top_row,
    int viewport_rows,
    int overscan_rows = 0) {
    ChatVisibleRows out;
    out.viewport_rows = std::max(0, viewport_rows);
    if (message_count <= 0 || viewport_rows <= 0) {
        return out;
    }

    const int total =
        chat_viewport_total_rows(message_row_counts, message_count);
    const int top = chat_viewport_clamp_scroll_top_row(
        scroll_top_row, total, viewport_rows);
    const int overscan = std::max(0, overscan_rows);
    out.row_begin = std::max(0, top - overscan);
    out.row_end = std::min(total, top + viewport_rows + overscan);
    if (out.row_end <= out.row_begin) {
        return out;
    }

    const auto layouts = chat_viewport_build_message_layouts(
        message_row_counts, message_count);
    for (const auto& layout : layouts) {
        const int content_begin = std::max(out.row_begin, layout.start_row);
        const int content_end = std::min(out.row_end, layout.end_row);
        const bool has_content = content_end > content_begin;
        const bool spacer_visible =
            out.row_begin <= layout.spacer_row &&
            layout.spacer_row < out.row_end;
        if (!has_content && !spacer_visible) {
            continue;
        }

        ChatRowRange range;
        range.message_index = layout.message_index;
        if (has_content) {
            range.row_begin = content_begin - layout.start_row;
            range.row_end = content_end - layout.start_row;
        } else {
            range.row_begin = layout.row_count;
            range.row_end = layout.row_count;
        }
        range.spacer_after_visible = spacer_visible;
        out.messages.push_back(range);
    }
    return out;
}

} // namespace acecode::tui
