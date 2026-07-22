#include "ask_question_overlay.hpp"

#include <ftxui/screen/string.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>

namespace acecode::tui {
namespace {

constexpr int kAskOverlayReservedTerminalRows = 12;
constexpr int kAskOverlayMinimumVisibleRows = 4;
constexpr int kAskOverlayMinimumContentWidth = 8;
// Full-width layout reserves the outer frame, overlay border, scrollbar, and
// the existing breathing room around the overlay text.
constexpr int kAskOverlayTerminalChromeWidth = 10;
// A reflected main-column box is already inside the outer frame, so only the
// overlay-local border, scrollbar, and breathing room remain.
constexpr int kAskOverlayMainColumnChromeWidth = 8;
constexpr int kOuterFrameHorizontalChromeWidth = 2;
constexpr int kSidebarSeparatorWidth = 1;

bool is_utf8_continuation(unsigned char c) {
    return (c & 0xC0) == 0x80;
}

std::size_t utf8_sequence_length(unsigned char c) {
    if ((c & 0x80) == 0x00) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

std::size_t next_codepoint_end(const std::string& s, std::size_t start) {
    if (start >= s.size()) return s.size();
    const std::size_t len = utf8_sequence_length(static_cast<unsigned char>(s[start]));
    const std::size_t end = start + len;
    if (end > s.size()) return start + 1;
    for (std::size_t i = start + 1; i < end; ++i) {
        if (!is_utf8_continuation(static_cast<unsigned char>(s[i]))) {
            return start + 1;
        }
    }
    return end;
}

bool is_ascii_space_glyph(const std::string& glyph) {
    return glyph == " " || glyph == "\t";
}

std::size_t skip_ascii_spaces(const std::string& s, std::size_t pos) {
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) {
        ++pos;
    }
    return pos;
}

std::string spaces_for_width(int width) {
    return std::string(static_cast<std::size_t>(std::max(0, width)), ' ');
}

struct WrapPart {
    std::string text;
    std::size_t next_pos = 0;
};

WrapPart take_wrapped_part(const std::string& text,
                           std::size_t pos,
                           int available_width) {
    available_width = std::max(1, available_width);
    pos = skip_ascii_spaces(text, pos);
    if (pos >= text.size()) {
        return {"", text.size()};
    }

    const std::size_t line_start = pos;
    std::size_t cur = pos;
    std::size_t last_good = line_start;
    std::size_t last_break = std::string::npos;
    std::size_t after_break = std::string::npos;
    int width = 0;

    while (cur < text.size()) {
        if (text[cur] == '\n') {
            return {text.substr(line_start, cur - line_start), cur + 1};
        }

        const std::size_t next = next_codepoint_end(text, cur);
        const std::string glyph = text.substr(cur, next - cur);
        const int glyph_width = std::max(0, display_width_cells(glyph));

        if (width + glyph_width > available_width) {
            if (last_break != std::string::npos && last_break > line_start) {
                return {text.substr(line_start, last_break - line_start),
                        skip_ascii_spaces(text, after_break)};
            }
            if (last_good > line_start) {
                return {text.substr(line_start, last_good - line_start), cur};
            }
            return {glyph, next};
        }

        width += glyph_width;
        last_good = next;
        if (is_ascii_space_glyph(glyph)) {
            last_break = cur;
            after_break = next;
        }
        cur = next;
    }

    return {text.substr(line_start), text.size()};
}

void append_wrapped_rows(AskOverlayLayout& layout,
                         AskOverlayRowKind kind,
                         const std::string& body,
                         const std::string& first_prefix,
                         const std::string& continuation_prefix,
                         int content_width,
                         int option_index,
                         bool focused_first_row) {
    const int first_available =
        std::max(1, content_width - display_width_cells(first_prefix));
    const int continuation_available =
        std::max(1, content_width - display_width_cells(continuation_prefix));

    if (body.empty()) {
        layout.rows.push_back({
            first_prefix,
            kind,
            option_index,
            focused_first_row,
            false,
        });
        return;
    }

    std::size_t pos = 0;
    bool first = true;
    while (pos < body.size()) {
        const std::string& prefix = first ? first_prefix : continuation_prefix;
        const int available = first ? first_available : continuation_available;
        WrapPart part = take_wrapped_part(body, pos, available);
        layout.rows.push_back({
            prefix + part.text,
            kind,
            option_index,
            focused_first_row && first,
            !first,
        });
        if (part.next_pos <= pos) {
            break;
        }
        pos = part.next_pos;
        first = false;
    }
}

void append_blank(AskOverlayLayout& layout) {
    layout.rows.push_back({"", AskOverlayRowKind::Blank, -1, false, false});
}

std::string ask_page_indicator(int current_page,
                               int total_questions,
                               const std::vector<bool>& answered_questions) {
    const int question_count = std::max(1, total_questions);
    const int total_pages = question_count + 1;
    current_page = std::clamp(current_page, 0, total_pages - 1);

    std::string markers = "[";
    for (int i = 0; i < total_pages; ++i) {
        if (i == current_page) {
            markers += "#";
        } else if (i == question_count) {
            markers += "S";
        } else if (i < static_cast<int>(answered_questions.size()) &&
                   answered_questions[i]) {
            markers += "*";
        } else {
            markers += ".";
        }
    }
    markers += "]";

    return std::to_string(current_page + 1) + "/" +
        std::to_string(total_pages) + " " + markers;
}

std::string header_with_indicator(const std::string& left,
                                  const std::string& indicator,
                                  int content_width) {
    const int left_width = display_width_cells(left);
    const int indicator_width = display_width_cells(indicator);
    if (left_width + 2 + indicator_width <= content_width) {
        return left + spaces_for_width(content_width - left_width - indicator_width) +
            indicator;
    }
    return left + "  " + indicator;
}

} // namespace

int display_width_cells(const std::string& text) {
    return std::max(0, ftxui::string_width(text));
}

int ask_overlay_content_width_for_frame(int terminal_width,
                                        int measured_main_column_width,
                                        bool regular_sidebar_visible,
                                        int regular_sidebar_width) {
    terminal_width = std::max(1, terminal_width);
    regular_sidebar_width = std::max(0, regular_sidebar_width);

    int estimated_main_column_width =
        terminal_width - kOuterFrameHorizontalChromeWidth;
    if (regular_sidebar_visible) {
        estimated_main_column_width -=
            regular_sidebar_width + kSidebarSeparatorWidth;
    }

    // reflect(chat_box) contains the previous rendered main-column width. On
    // the first frame Box{} reports one cell; after a resize it can also still
    // contain the wider, single-column box. The current composition estimate
    // is therefore always an upper bound, while a valid narrower measurement
    // can keep wrapping conservative until the next frame catches up.
    int main_column_width = estimated_main_column_width;
    if (measured_main_column_width > kAskOverlayMainColumnChromeWidth) {
        main_column_width = std::min(
            measured_main_column_width, estimated_main_column_width);
    }

    const int terminal_bound = std::max(
        kAskOverlayMinimumContentWidth,
        terminal_width - kAskOverlayTerminalChromeWidth);
    const int main_column_bound = std::max(
        kAskOverlayMinimumContentWidth,
        main_column_width - kAskOverlayMainColumnChromeWidth);
    return std::min(terminal_bound, main_column_bound);
}

AskOverlayLayout build_ask_overlay_layout(const AskOverlayLayoutInput& input) {
    AskOverlayLayout layout;
    if (!input.submit_page && input.question == nullptr) {
        return layout;
    }

    const int content_width =
        std::max(kAskOverlayMinimumContentWidth, input.content_width);
    const int question_count = std::max(1, input.total_questions);
    const int current_page = input.submit_page
        ? question_count
        : std::clamp(input.current_question_index, 0, question_count - 1);
    const std::string indicator =
        ask_page_indicator(current_page, question_count,
                           input.answered_questions);

    if (input.submit_page) {
        const std::string header =
            header_with_indicator(" Submit", indicator, content_width);
        append_wrapped_rows(layout, AskOverlayRowKind::Header, header, "", "",
                            content_width, -1, false);
        append_wrapped_rows(layout, AskOverlayRowKind::Body,
                            "Ready to submit?", " ", " ", content_width,
                            -1, false);
        append_blank(layout);

        const std::array<std::string, 2> labels = {
            "Submit answers",
            "Cancel",
        };
        const int focus = std::clamp(input.submit_focus, 0, 1);
        for (int i = 0; i < static_cast<int>(labels.size()); ++i) {
            const bool focused = (i == focus);
            const std::string marker = std::to_string(i + 1) + ". ";
            const std::string first_prefix = focused
                ? " \xE2\x96\xB8 " + marker
                : "   " + marker;
            const std::string continuation_prefix =
                spaces_for_width(display_width_cells(first_prefix));

            const int row_begin = static_cast<int>(layout.rows.size());
            append_wrapped_rows(layout, AskOverlayRowKind::Option, labels[i],
                                first_prefix, continuation_prefix,
                                content_width, i, focused);
            const int row_end = static_cast<int>(layout.rows.size()) - 1;
            if (focused) {
                layout.focused_row_begin = row_begin;
                layout.focused_row_end = row_end;
            }
        }

        append_blank(layout);
        append_wrapped_rows(layout, AskOverlayRowKind::Hint,
                            " \xE2\x86\x91\xE2\x86\x93 move   Enter select   Esc cancel",
                            "", "", content_width, -1, false);
        return layout;
    }

    const AskQuestion& q = *input.question;
    const int option_count = static_cast<int>(q.options.size());
    const int total_rows = option_count + 1;
    const int focus = std::clamp(input.option_focus, 0, std::max(0, total_rows - 1));

    const std::string header_left = " Question " +
        std::to_string(input.current_question_index + 1) + "/" +
        std::to_string(std::max(1, input.total_questions)) +
        "  [" + q.header + "]";
    const std::string header =
        header_with_indicator(header_left, indicator, content_width);
    append_wrapped_rows(layout, AskOverlayRowKind::Header, header, "", "",
                        content_width, -1, false);
    append_wrapped_rows(layout, AskOverlayRowKind::Body, q.question, " ", " ",
                        content_width, -1, false);
    append_blank(layout);

    for (int i = 0; i < total_rows; ++i) {
        const bool is_other = (i == option_count);
        const bool focused = (i == focus);
        std::string marker;
        if (q.multi_select) {
            const bool checked =
                is_other
                    ? (input.question_answered &&
                       input.selected_option == i)
                    : (i < static_cast<int>(input.multi_selected.size()) &&
                       input.multi_selected[i]);
            marker = checked ? "[x] " : "[ ] ";
        } else {
            const bool selected = input.question_answered
                ? input.selected_option == i
                : focused;
            marker = selected ? "(\xE2\x97\x8F) " : "( ) ";
        }

        const std::string first_prefix = focused
            ? " \xE2\x96\xB8 " + marker
            : "   " + marker;
        const std::string continuation_prefix =
            spaces_for_width(display_width_cells(first_prefix));

        std::string body;
        if (is_other) {
            body = "Other...";
        } else {
            body = q.options[i].label;
            if (!q.options[i].description.empty()) {
                body += "  ";
                body += q.options[i].description;
            }
        }

        const int row_begin = static_cast<int>(layout.rows.size());
        append_wrapped_rows(layout, AskOverlayRowKind::Option, body,
                            first_prefix, continuation_prefix, content_width,
                            i, focused);
        const int row_end = static_cast<int>(layout.rows.size()) - 1;
        if (focused) {
            layout.focused_row_begin = row_begin;
            layout.focused_row_end = row_end;
        }
    }

    append_blank(layout);

    const std::string hint = q.multi_select
        ? " \xE2\x86\x91\xE2\x86\x93 move   Space toggle   Enter submit   Esc cancel"
        : " \xE2\x86\x91\xE2\x86\x93 move   Enter select   Esc cancel";
    append_wrapped_rows(layout, AskOverlayRowKind::Hint, hint, "", "",
                        content_width, -1, false);

    if (input.timeout_hint_seconds > 0) {
        append_wrapped_rows(layout, AskOverlayRowKind::Hint,
                            " " + std::to_string(input.timeout_hint_seconds) +
                                "s 无操作将自动选择推荐项",
                            "", "", content_width, -1, false);
    }

    if (input.other_input_active) {
        append_blank(layout);
        append_wrapped_rows(layout, AskOverlayRowKind::CustomPrompt,
                            " Custom answer (Enter to submit, Esc to back out):",
                            "", "", content_width, -1, false);
    }

    return layout;
}

int ask_overlay_visible_rows_for_terminal(int terminal_rows) {
    return std::max(kAskOverlayMinimumVisibleRows,
                    terminal_rows - kAskOverlayReservedTerminalRows);
}

int clamp_scroll_offset(int offset, int total_rows, int visible_rows) {
    if (total_rows <= 0 || visible_rows <= 0 || total_rows <= visible_rows) {
        return 0;
    }
    return std::clamp(offset, 0, total_rows - visible_rows);
}

int scroll_offset_by_lines(int offset, int delta, int total_rows, int visible_rows) {
    return clamp_scroll_offset(offset + delta, total_rows, visible_rows);
}

int ensure_row_range_visible(int offset,
                             int visible_rows,
                             int total_rows,
                             int row_begin,
                             int row_end) {
    if (total_rows <= 0 || visible_rows <= 0 || total_rows <= visible_rows) {
        return 0;
    }

    row_begin = std::clamp(row_begin, 0, total_rows - 1);
    row_end = std::clamp(row_end, row_begin, total_rows - 1);
    offset = clamp_scroll_offset(offset, total_rows, visible_rows);

    if (row_begin < offset) {
        offset = row_begin;
    } else if (row_end >= offset + visible_rows) {
        offset = row_end - visible_rows + 1;
    }

    return clamp_scroll_offset(offset, total_rows, visible_rows);
}

int scroll_offset_for_track_y(int mouse_y,
                              int track_y_min,
                              int track_height,
                              int total_rows,
                              int visible_rows) {
    if (total_rows <= 0 || visible_rows <= 0 || total_rows <= visible_rows) {
        return 0;
    }

    const int max_offset = total_rows - visible_rows;
    if (track_height <= 1) {
        return 0;
    }

    int rel = mouse_y - track_y_min;
    rel = std::clamp(rel, 0, track_height - 1);
    return static_cast<int>(
        static_cast<long long>(rel) * max_offset / (track_height - 1));
}

} // namespace acecode::tui
