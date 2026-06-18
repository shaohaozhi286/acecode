#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace acecode::tui {

struct ChatLineMeasure {
    int rows = 1;
    bool valid = false;
    int width = 0;
    std::size_t revision = 0;
};

inline int chat_line_measure_rows(const ChatLineMeasure& measure) {
    return measure.rows > 0 ? measure.rows : 1;
}

inline void resize_chat_line_measures(std::vector<ChatLineMeasure>& measures,
                                      int message_count) {
    measures.resize(static_cast<std::size_t>(std::max(0, message_count)));
}

inline void invalidate_chat_line_measure(
    std::vector<ChatLineMeasure>& measures,
    int index) {
    if (index < 0 || index >= static_cast<int>(measures.size())) {
        return;
    }
    measures[static_cast<std::size_t>(index)].rows = 1;
    measures[static_cast<std::size_t>(index)].valid = false;
}

inline void invalidate_chat_line_measures(
    std::vector<ChatLineMeasure>& measures) {
    for (auto& measure : measures) {
        measure.rows = 1;
        measure.valid = false;
    }
}

inline bool sync_chat_line_measure(ChatLineMeasure& measure,
                                   bool has_layout_measure,
                                   int measured_rows,
                                   int width,
                                   std::size_t revision) {
    const bool width_changed =
        width > 0 && measure.width > 0 && measure.width != width;
    const bool revision_changed = measure.revision != revision;
    if (width_changed || revision_changed) {
        measure.rows = 1;
        measure.valid = false;
    }

    if (width > 0) {
        measure.width = width;
    }
    measure.revision = revision;

    if (has_layout_measure && measured_rows > 0) {
        measure.rows = std::max(1, measured_rows);
        measure.valid = true;
        return true;
    }

    if (measure.rows <= 0) {
        measure.rows = 1;
    }
    return false;
}

inline std::vector<int> chat_line_counts_from_measures(
    const std::vector<ChatLineMeasure>& measures,
    int message_count) {
    message_count = std::max(0, message_count);
    std::vector<int> counts;
    counts.reserve(static_cast<std::size_t>(message_count));
    for (int i = 0; i < message_count; ++i) {
        if (i < static_cast<int>(measures.size())) {
            counts.push_back(chat_line_measure_rows(
                measures[static_cast<std::size_t>(i)]));
        } else {
            counts.push_back(1);
        }
    }
    return counts;
}

} // namespace acecode::tui
