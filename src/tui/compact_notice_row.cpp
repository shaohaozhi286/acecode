#include "tui/compact_notice_row.hpp"

#include "session/compact_notice.hpp"

namespace acecode { namespace tui {

bool append_compact_notice_row(std::vector<TuiState::Message>& rows,
                               const ChatMessage& message) {
    const auto notice = decode_compact_notice(message);
    if (!notice.has_value()) return false;

    TuiState::Message* row = nullptr;
    if (!rows.empty() &&
        rows.back().role == "compact_notice" &&
        rows.back().compact_notice_id == notice->id) {
        row = &rows.back();
        if (!message.content.empty()) {
            if (!row->content.empty()) row->content += "\n\n";
            row->content += message.content;
        }
    } else {
        TuiState::Message compact_row;
        compact_row.role = "compact_notice";
        compact_row.content = message.content;
        compact_row.compact_notice_id = notice->id;
        rows.push_back(std::move(compact_row));
        row = &rows.back();
    }

    row->compact_notice_complete =
        row->compact_notice_complete || notice->complete;
    row->expanded = !row->compact_notice_complete;
    return true;
}

bool compact_notice_row_is_expanded(const TuiState::Message& row,
                                    bool transcript_expanded) {
    return row.role == "compact_notice" &&
        (!row.compact_notice_complete || row.expanded || transcript_expanded);
}

bool toggle_completed_compact_notice_row(TuiState::Message& row) {
    if (row.role != "compact_notice" || !row.compact_notice_complete) {
        return false;
    }
    row.expanded = !row.expanded;
    return true;
}

}} // namespace acecode::tui
