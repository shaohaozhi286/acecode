#pragma once

#include "session/todo_state.hpp"
#include "tui/text_truncation.hpp"
#include "tui/text_style.hpp"
#include "tui/theme_palette.hpp"

#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace acecode::tui {

struct TodoChecklistRowPresentation {
    std::string marker;
    std::vector<std::string> content_lines;
    bool marker_active = false;
    bool row_bright = false;
    bool strike = false;
    bool muted = false;
};

inline constexpr std::size_t kTodoChecklistMaxVisibleItems = 10;
inline constexpr std::size_t kTodoChecklistMaxContentLines = 2;

inline const char* todo_check_marker() {
    return "\xE2\x9C\x94"; // check mark
}

inline const char* todo_filled_square_marker() {
    return "\xE2\x97\xBC"; // black medium square
}

inline const char* todo_open_square_marker() {
    return "\xE2\x97\xBB"; // white medium square
}

inline int todo_status_priority(const std::string& normalized_status) {
    if (normalized_status == "in_progress") return 0;
    if (normalized_status == "pending") return 1;
    if (normalized_status == "completed") return 2;
    if (normalized_status == "cancelled") return 3;
    return 1;
}

inline std::vector<TodoChecklistRowPresentation> todo_checklist_rows(
    const std::vector<TodoItem>& todos,
    int content_width,
    std::size_t max_visible = kTodoChecklistMaxVisibleItems) {
    std::vector<TodoChecklistRowPresentation> rows;
    std::vector<std::size_t> order(todos.size());
    for (std::size_t i = 0; i < todos.size(); ++i) {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(),
        [&](std::size_t lhs, std::size_t rhs) {
            return todo_status_priority(normalize_todo_status(todos[lhs].status)) <
                   todo_status_priority(normalize_todo_status(todos[rhs].status));
        });

    const std::size_t visible = std::min(max_visible, order.size());
    rows.reserve(visible);
    for (std::size_t i = 0; i < visible; ++i) {
        const TodoItem& item = todos[order[i]];
        const std::string status = normalize_todo_status(item.status);
        TodoChecklistRowPresentation row;
        row.marker = todo_open_square_marker();
        row.content_lines = wrap_truncate_end(
            item.content,
            std::max(1, content_width),
            kTodoChecklistMaxContentLines);
        if (row.content_lines.empty()) {
            row.content_lines.push_back("");
        }

        if (status == "in_progress") {
            row.marker = todo_filled_square_marker();
            row.marker_active = true;
            row.row_bright = true;
        } else if (status == "completed") {
            row.marker = todo_check_marker();
            row.strike = true;
        } else if (status == "cancelled") {
            row.muted = true;
            row.strike = true;
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

inline bool todo_checklist_uses_sidebar(bool regular_sidebar_visible) {
    return regular_sidebar_visible;
}

inline ftxui::Element render_todo_checklist_block(
    const std::vector<TodoItem>& todos,
    int available_width) {
    using namespace ftxui;
    if (todos.empty()) {
        return emptyElement();
    }

    const std::string marker_prefix =
        std::string(todo_open_square_marker()) + " ";
    const int content_width = std::max(
        1, available_width - visual_width(marker_prefix));
    Elements rows;
    for (const auto& item : todo_checklist_rows(todos, content_width)) {
        Element marker = text(item.marker + " ");
        Elements content_lines;
        content_lines.reserve(item.content_lines.size());
        for (const auto& line : item.content_lines) {
            Element line_el = text(line);
            if (item.row_bright) {
                line_el = line_el | color(theme().ui.text_primary);
            }
            if (item.strike) {
                line_el = line_el | strikethrough | color(theme().ui.text_muted);
            }
            if (item.muted) {
                line_el = line_el | readable_secondary();
            }
            content_lines.push_back(std::move(line_el));
        }
        Element content = vbox(std::move(content_lines)) | flex;
        if (item.marker_active) {
            marker = marker | color(theme().semantic.success);
        }
        if (item.muted) {
            marker = marker | readable_secondary();
        }
        Element row = hbox({std::move(marker), std::move(content)});
        rows.push_back(std::move(row));
    }

    if (todos.size() > kTodoChecklistMaxVisibleItems) {
        rows.push_back(
            text("+" + std::to_string(todos.size() - kTodoChecklistMaxVisibleItems) +
                 " more") |
            readable_secondary());
    }
    return vbox(std::move(rows));
}

} // namespace acecode::tui
