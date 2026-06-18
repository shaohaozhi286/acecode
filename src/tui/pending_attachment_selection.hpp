#pragma once

#include <cstddef>
#include <optional>

namespace acecode::tui {

constexpr int kNoPendingAttachmentFocus = -1;

bool has_pending_attachment_focus(int selected_index, std::size_t attachment_count);
bool clamp_pending_attachment_focus(int& selected_index, std::size_t attachment_count);
bool toggle_pending_attachment_focus(int& selected_index, std::size_t attachment_count);
bool move_pending_attachment_focus(int& selected_index,
                                   std::size_t attachment_count,
                                   int delta);
std::optional<std::size_t> remove_focused_pending_attachment_index(
    int& selected_index,
    std::size_t attachment_count);

} // namespace acecode::tui
