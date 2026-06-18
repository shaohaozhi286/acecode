#include "pending_attachment_selection.hpp"

#include <algorithm>

namespace acecode::tui {

bool has_pending_attachment_focus(int selected_index, std::size_t attachment_count) {
    return selected_index >= 0 &&
           static_cast<std::size_t>(selected_index) < attachment_count;
}

bool clamp_pending_attachment_focus(int& selected_index, std::size_t attachment_count) {
    const int before = selected_index;
    if (attachment_count == 0) {
        selected_index = kNoPendingAttachmentFocus;
    } else if (selected_index >= 0) {
        const int last = static_cast<int>(attachment_count - 1);
        selected_index = std::clamp(selected_index, 0, last);
    } else {
        selected_index = kNoPendingAttachmentFocus;
    }
    return selected_index != before;
}

bool toggle_pending_attachment_focus(int& selected_index, std::size_t attachment_count) {
    const int before = selected_index;
    if (attachment_count == 0) {
        selected_index = kNoPendingAttachmentFocus;
    } else if (has_pending_attachment_focus(selected_index, attachment_count)) {
        selected_index = kNoPendingAttachmentFocus;
    } else {
        selected_index = static_cast<int>(attachment_count - 1);
    }
    return selected_index != before;
}

bool move_pending_attachment_focus(int& selected_index,
                                   std::size_t attachment_count,
                                   int delta) {
    if (!has_pending_attachment_focus(selected_index, attachment_count)) {
        clamp_pending_attachment_focus(selected_index, attachment_count);
        return false;
    }
    const int before = selected_index;
    const int last = static_cast<int>(attachment_count - 1);
    selected_index = std::clamp(selected_index + delta, 0, last);
    return selected_index != before;
}

std::optional<std::size_t> remove_focused_pending_attachment_index(
    int& selected_index,
    std::size_t attachment_count) {
    if (!has_pending_attachment_focus(selected_index, attachment_count)) {
        clamp_pending_attachment_focus(selected_index, attachment_count);
        return std::nullopt;
    }

    const std::size_t removed = static_cast<std::size_t>(selected_index);
    const std::size_t remaining = attachment_count - 1;
    if (remaining == 0) {
        selected_index = kNoPendingAttachmentFocus;
    } else if (removed >= remaining) {
        selected_index = static_cast<int>(remaining - 1);
    }
    return removed;
}

} // namespace acecode::tui
