#include "chat_viewport.hpp"

#include "tui/chat_scroll.hpp"
#include "utils/logger.hpp"

#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/dom/requirement.hpp>
#include <ftxui/screen/color.hpp>
#include <ftxui/screen/string.hpp>

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>

namespace acecode::tui {
namespace {

ftxui::Box empty_box() {
    ftxui::Box box;
    box.x_min = 1;
    box.x_max = 0;
    box.y_min = 1;
    box.y_max = 0;
    return box;
}

bool valid_box(const ftxui::Box& box) {
    return box.x_min <= box.x_max && box.y_min <= box.y_max;
}

bool contains_box(const ftxui::Box& box, int x, int y) {
    return valid_box(box) && box.Contain(x, y);
}

bool boxes_equal(const ftxui::Box& a, const ftxui::Box& b) {
    return a.x_min == b.x_min &&
           a.x_max == b.x_max &&
           a.y_min == b.y_min &&
           a.y_max == b.y_max;
}

std::string format_box(const ftxui::Box& box) {
    if (!valid_box(box)) {
        return "-";
    }
    return std::to_string(box.x_min) + "," + std::to_string(box.y_min) +
        "-" + std::to_string(box.x_max) + "," +
        std::to_string(box.y_max);
}

int box_width(const ftxui::Box& box) {
    return valid_box(box) ? box.x_max - box.x_min + 1 : 0;
}

int box_height(const ftxui::Box& box) {
    return valid_box(box) ? box.y_max - box.y_min + 1 : 0;
}

void apply_style(ftxui::Cell& cell, ChatViewportRowStyle style) {
    using ftxui::Color;
    switch (style) {
    case ChatViewportRowStyle::User:
        cell.foreground_color = Color::White;
        break;
    case ChatViewportRowStyle::Assistant:
        cell.foreground_color = Color::GreenLight;
        break;
    case ChatViewportRowStyle::ToolCall:
        cell.foreground_color = Color::MagentaLight;
        cell.dim = true;
        break;
    case ChatViewportRowStyle::ToolResult:
        cell.foreground_color = Color::GrayLight;
        break;
    case ChatViewportRowStyle::System:
        cell.foreground_color = Color::Yellow;
        break;
    case ChatViewportRowStyle::Error:
        cell.foreground_color = Color::RedLight;
        break;
    }
}

int write_glyphs(ftxui::Screen& screen,
                 int x,
                 int y,
                 int x_max,
                 const std::string& text,
                 ChatViewportRowStyle style) {
    if (y < screen.stencil.y_min || y > screen.stencil.y_max) {
        return x;
    }

    for (const auto& glyph : ftxui::Utf8ToGlyphs(text)) {
        const int width = std::max(1, ftxui::string_width(glyph));
        if (x + width - 1 > x_max) {
            break;
        }
        if (x >= screen.stencil.x_min && x <= screen.stencil.x_max) {
            auto& cell = screen.CellAt(x, y);
            cell.character = glyph.empty() ? " " : glyph;
            apply_style(cell, style);
        }
        for (int dx = 1; dx < width; ++dx) {
            const int continuation_x = x + dx;
            if (continuation_x >= screen.stencil.x_min &&
                continuation_x <= screen.stencil.x_max) {
                auto& cell = screen.CellAt(continuation_x, y);
                cell.character = " ";
                apply_style(cell, style);
            }
        }
        x += width;
    }
    return x;
}

class ChatViewportNode : public ftxui::Node {
public:
    explicit ChatViewportNode(ChatViewport* owner) : owner_(owner) {}

    void ComputeRequirement() override {
        requirement_.min_x = 1;
        requirement_.min_y = 1;
        requirement_.flex_grow_x = 1;
        requirement_.flex_grow_y = 1;
        requirement_.flex_shrink_x = 1;
        requirement_.flex_shrink_y = 1;
    }

    void SetBox(ftxui::Box box) override {
        ftxui::Node::SetBox(box);
        owner_->refresh_layout_for_box(box);
    }

    void Render(ftxui::Screen& screen) override {
        owner_->render_to_screen(screen);
    }

private:
    ChatViewport* owner_;
};

} // namespace

ChatViewport::ChatViewport()
    : outer_box_(empty_box()),
      chat_box_(empty_box()),
      scrollbar_box_(empty_box()) {}

ChatViewport::ChatViewport(ChatViewportOptions options)
    : options_(options),
      outer_box_(empty_box()),
      chat_box_(empty_box()),
      scrollbar_box_(empty_box()) {}

ftxui::Element ChatViewport::OnRender() {
    return std::make_shared<ChatViewportNode>(this);
}

bool ChatViewport::OnEvent(ftxui::Event event) {
    if (!event.is_mouse()) {
        if (!has_chat_box()) {
            return false;
        }
        if (event == ftxui::Event::PageUp ||
            event == ftxui::Event::PageDown) {
            const int step = std::max(1, state_.viewport_rows - 2);
            scroll_by_rows(event == ftxui::Event::PageUp ? -step : step);
            return true;
        }
        if (event == ftxui::Event::Special("\x1B[1;3A") ||
            event == ftxui::Event::Special("\x1B[1;3B")) {
            scroll_by_rows(event == ftxui::Event::Special("\x1B[1;3A")
                               ? -1
                               : 1);
            return true;
        }
        return false;
    }

    auto& mouse = event.mouse();
    const bool wheel = mouse.button == ftxui::Mouse::WheelUp ||
                       mouse.button == ftxui::Mouse::WheelDown;
    const bool hit_chat = contains_box(chat_box_, mouse.x, mouse.y);
    const bool hit_scrollbar = contains_box(scrollbar_box_, mouse.x, mouse.y);

    if (scrollbar_dragging_) {
        if (mouse.motion == ftxui::Mouse::Released) {
            end_scrollbar_drag();
            return true;
        }
        if (mouse.motion == ftxui::Mouse::Moved) {
            move_scrollbar_drag(mouse.y);
            return true;
        }
    }

    if (!hit_chat && !hit_scrollbar) {
        return false;
    }

    if (wheel) {
        const int delta = mouse.button == ftxui::Mouse::WheelUp
            ? -std::max(1, options_.wheel_lines)
            : std::max(1, options_.wheel_lines);
        scroll_by_rows(delta);
        return true;
    }

    if (mouse.button == ftxui::Mouse::Left &&
        mouse.motion == ftxui::Mouse::Pressed &&
        hit_scrollbar) {
        begin_scrollbar_drag(mouse.y);
        return true;
    }

    if (mouse.button == ftxui::Mouse::Left &&
        mouse.motion == ftxui::Mouse::Pressed &&
        hit_chat) {
        return false;
    }

    return false;
}

void ChatViewport::set_messages(
    std::vector<ChatViewportMessageInput> messages) {
    if (messages == messages_) {
        return;
    }
    if (messages.size() < messages_.size()) {
        cache_.clear();
    }
    messages_ = std::move(messages);
    layout_dirty_ = true;
    refresh_layout_for_box(outer_box_);
}

void ChatViewport::set_options(ChatViewportOptions options) {
    const bool changed =
        options.scrollbar_width != options_.scrollbar_width ||
        options.wheel_lines != options_.wheel_lines ||
        options.overscan_rows != options_.overscan_rows ||
        options.debug_trace != options_.debug_trace;
    if (!changed) {
        return;
    }
    options_ = options;
    layout_dirty_ = true;
    refresh_layout_for_box(outer_box_);
}

int ChatViewport::scroll_by_rows(int delta_rows) {
    return chat_viewport_scroll_by_rows(state_, delta_rows);
}

int ChatViewport::scroll_to_row(int row) {
    return chat_viewport_scroll_to_row(state_, row);
}

int ChatViewport::scroll_to_tail() {
    return chat_viewport_scroll_to_tail(state_);
}

bool ChatViewport::is_at_tail() const {
    return chat_viewport_is_at_tail(state_);
}

ChatDisplayRow ChatViewport::top_display_row() const {
    return chat_viewport_display_row_at(
        message_row_counts_, static_cast<int>(messages_.size()),
        state_.scroll_top_row);
}

std::pair<int, int> ChatViewport::visible_message_index_bounds() const {
    const auto visible = chat_viewport_visible_rows(
        message_row_counts_, static_cast<int>(messages_.size()),
        state_.scroll_top_row, state_.viewport_rows, options_.overscan_rows);
    int first = -1;
    int last = -1;
    for (const auto& range : visible.messages) {
        if (range.message_index < 0) {
            continue;
        }
        if (first < 0) {
            first = range.message_index;
        }
        last = range.message_index;
    }
    return {first, last};
}

const ChatViewportState& ChatViewport::state() const {
    return state_;
}

const ftxui::Box& ChatViewport::chat_box() const {
    return chat_box_;
}

const ftxui::Box& ChatViewport::scrollbar_box() const {
    return scrollbar_box_;
}

ChatViewportCacheStats ChatViewport::cache_stats() const {
    return cache_.stats();
}

void ChatViewport::refresh_layout_for_box(ftxui::Box box) {
    if (!layout_dirty_ && boxes_equal(box, outer_box_)) {
        return;
    }
    rebuild_layout_for_box(box);
}

void ChatViewport::rebuild_layout_for_box(ftxui::Box box) {
    outer_box_ = box;
    if (!valid_box(box)) {
        chat_box_ = empty_box();
        scrollbar_box_ = empty_box();
        message_row_counts_.clear();
        chat_viewport_set_metrics(state_, 0, 0);
        layout_dirty_ = false;
        trace_viewport("layout-empty");
        return;
    }

    const int total_width = box_width(box);
    const int scrollbar_width =
        total_width > 1
            ? std::clamp(options_.scrollbar_width, 1, total_width)
            : 0;

    chat_box_ = box;
    chat_box_.x_max = box.x_max - scrollbar_width;
    if (!valid_box(chat_box_)) {
        chat_box_ = empty_box();
    }

    if (scrollbar_width > 0) {
        scrollbar_box_ = box;
        scrollbar_box_.x_min = box.x_max - scrollbar_width + 1;
    } else {
        scrollbar_box_ = empty_box();
    }

    const int content_width = std::max(1, box_width(chat_box_));
    message_row_counts_.clear();
    message_row_counts_.reserve(messages_.size());
    for (std::size_t i = 0; i < messages_.size(); ++i) {
        const int row_count = cache_.row_count_for_message(
            static_cast<int>(i), messages_[i], content_width);
        message_row_counts_.push_back(row_count);
    }

    const int total_rows = chat_viewport_total_rows(
        message_row_counts_, static_cast<int>(message_row_counts_.size()));
    chat_viewport_set_metrics(state_, total_rows, box_height(chat_box_));
    layout_dirty_ = false;
    trace_viewport("layout");
}

void ChatViewport::render_to_screen(ftxui::Screen& screen) {
    if (!has_chat_box()) {
        return;
    }

    const int message_count = static_cast<int>(messages_.size());
    const int viewport_rows = box_height(chat_box_);
    const int top_padding =
        state_.total_rows > 0 && state_.total_rows < viewport_rows
            ? viewport_rows - state_.total_rows
            : 0;
    const auto visible = chat_viewport_visible_rows(
        message_row_counts_, message_count, state_.scroll_top_row,
        viewport_rows, options_.overscan_rows);
    for (const auto& range : visible.messages) {
        if (!range.has_content_rows()) {
            continue;
        }
        const auto& layout = cache_.layout_for_message(
            range.message_index, messages_[range.message_index],
            std::max(1, box_width(chat_box_)));
        for (int message_row = range.row_begin;
             message_row < range.row_end;
             ++message_row) {
            if (message_row < 0 || message_row >= layout.row_count()) {
                continue;
            }
            const int absolute_row = chat_viewport_row_for_message_line(
                message_row_counts_, message_count, range.message_index,
                message_row);
            const int y_offset = absolute_row - state_.scroll_top_row;
            if (y_offset < 0 || y_offset >= viewport_rows - top_padding) {
                continue;
            }

            const auto& cached_row = layout.rows[message_row];
            const int y = chat_box_.y_min + top_padding + y_offset;
            int x = chat_box_.x_min;
            x = write_glyphs(screen, x, y, chat_box_.x_max,
                             cached_row.prefix, cached_row.style);
            write_glyphs(screen, x, y, chat_box_.x_max, cached_row.text,
                         cached_row.style);
        }
    }

    if (!has_scrollbar_box()) {
        return;
    }

    for (int y = scrollbar_box_.y_min; y <= scrollbar_box_.y_max; ++y) {
        for (int x = scrollbar_box_.x_min; x <= scrollbar_box_.x_max; ++x) {
            if (x >= screen.stencil.x_min && x <= screen.stencil.x_max &&
                y >= screen.stencil.y_min && y <= screen.stencil.y_max) {
                screen.CellAt(x, y).character = " ";
            }
        }
    }

    const int max_top = chat_viewport_max_scroll_top_row(
        state_.total_rows, state_.viewport_rows);
    if (max_top <= 0) {
        return;
    }

    const int track_height = box_height(scrollbar_box_);
    const int track_2x = 2 * track_height;
    const int min_thumb_2x = std::min(6, track_2x);
    int thumb_size_2x =
        static_cast<int>(static_cast<long long>(track_2x) * track_height /
                         std::max(1, state_.total_rows));
    thumb_size_2x = std::clamp(thumb_size_2x, min_thumb_2x, track_2x);
    const int range_2x = track_2x - thumb_size_2x;
    int thumb_top_2x = 2 * scrollbar_box_.y_min;
    if (range_2x > 0) {
        thumb_top_2x += static_cast<int>(
            static_cast<long long>(range_2x) * state_.scroll_top_row /
            max_top);
    }

    const int x = scrollbar_box_.x_max;
    for (int y = scrollbar_box_.y_min; y <= scrollbar_box_.y_max; ++y) {
        const int y_up = 2 * y;
        const int y_down = 2 * y + 1;
        const bool up =
            thumb_top_2x <= y_up && y_up <= thumb_top_2x + thumb_size_2x;
        const bool down =
            thumb_top_2x <= y_down && y_down <= thumb_top_2x + thumb_size_2x;
        if (!up && !down) {
            continue;
        }
        if (x >= screen.stencil.x_min && x <= screen.stencil.x_max &&
            y >= screen.stencil.y_min && y <= screen.stencil.y_max) {
            auto& cell = screen.CellAt(x, y);
            cell.character = "|";
            cell.foreground_color = ftxui::Color::GrayLight;
        }
    }
    trace_viewport("render");
}

bool ChatViewport::has_chat_box() const {
    return valid_box(chat_box_);
}

bool ChatViewport::has_scrollbar_box() const {
    return valid_box(scrollbar_box_);
}

int ChatViewport::scrollbar_y_to_row(int mouse_y) const {
    const int max_top = chat_viewport_max_scroll_top_row(
        state_.total_rows, state_.viewport_rows);
    const int track_height = box_height(scrollbar_box_);
    if (max_top <= 0 || track_height <= 1) {
        return 0;
    }

    const int rel = std::clamp(mouse_y - scrollbar_box_.y_min,
                               0, track_height - 1);
    return static_cast<int>(
        static_cast<long long>(rel) * max_top / (track_height - 1));
}

void ChatViewport::begin_scrollbar_drag(int mouse_y) {
    drag_row_counts_snapshot_ = message_row_counts_;
    scrollbar_dragging_ = true;

    const int track_height = box_height(scrollbar_box_);
    const int message_count =
        static_cast<int>(drag_row_counts_snapshot_.size());
    const int max_top = chat_max_scroll_top_row(
        drag_row_counts_snapshot_, message_count, state_.viewport_rows);
    const int current_top = state_.follow_tail ? max_top : state_.scroll_top_row;
    const auto geometry = chat_scrollbar_thumb_geometry(
        scrollbar_box_.y_min, track_height, drag_row_counts_snapshot_,
        message_count, state_.viewport_rows, current_top);
    drag_scrollbar_grab_offset_2x_ =
        chat_scrollbar_grab_offset_2x(mouse_y, geometry);
    state_.scroll_top_row = chat_scrollbar_y_to_top_row_with_grab(
        mouse_y, scrollbar_box_.y_min, geometry,
        drag_scrollbar_grab_offset_2x_);
    state_.target_scroll_top_row = state_.scroll_top_row;
    state_.follow_tail = state_.scroll_top_row >= max_top;
}

void ChatViewport::move_scrollbar_drag(int mouse_y) {
    const int track_height = box_height(scrollbar_box_);
    const int message_count =
        static_cast<int>(drag_row_counts_snapshot_.size());
    const auto geometry = chat_scrollbar_thumb_geometry(
        scrollbar_box_.y_min, track_height, drag_row_counts_snapshot_,
        message_count, state_.viewport_rows, state_.scroll_top_row);
    state_.scroll_top_row = chat_scrollbar_y_to_top_row_with_grab(
        mouse_y, scrollbar_box_.y_min, geometry,
        drag_scrollbar_grab_offset_2x_);
    state_.target_scroll_top_row = state_.scroll_top_row;
    state_.follow_tail = state_.scroll_top_row >= geometry.max_top_row;
}

void ChatViewport::end_scrollbar_drag() {
    scrollbar_dragging_ = false;
    drag_row_counts_snapshot_.clear();
    drag_scrollbar_grab_offset_2x_ = 0;
}

void ChatViewport::trace_viewport(const char* phase) const {
    if (!options_.debug_trace) {
        return;
    }

    const int message_count = static_cast<int>(message_row_counts_.size());
    const auto visible = chat_viewport_visible_rows(
        message_row_counts_, message_count, state_.scroll_top_row,
        state_.viewport_rows, options_.overscan_rows);
    const auto stats = cache_.stats();

    TraceSnapshot snapshot;
    snapshot.scroll_top_row = state_.scroll_top_row;
    snapshot.viewport_rows = state_.viewport_rows;
    snapshot.total_rows = state_.total_rows;
    snapshot.row_begin = visible.row_begin;
    snapshot.row_end = visible.row_end;
    snapshot.visible_messages = static_cast<int>(visible.messages.size());
    snapshot.chat_x_min = chat_box_.x_min;
    snapshot.chat_y_min = chat_box_.y_min;
    snapshot.chat_x_max = chat_box_.x_max;
    snapshot.chat_y_max = chat_box_.y_max;
    snapshot.scrollbar_x_min = scrollbar_box_.x_min;
    snapshot.scrollbar_y_min = scrollbar_box_.y_min;
    snapshot.scrollbar_x_max = scrollbar_box_.x_max;
    snapshot.scrollbar_y_max = scrollbar_box_.y_max;
    snapshot.cache_misses = stats.misses;
    snapshot.cache_builds = stats.builds;
    snapshot.row_count_misses = stats.row_count_misses;
    snapshot.row_count_builds = stats.row_count_builds;
    snapshot.cache_size = cache_.size();

    const bool same_trace =
        snapshot.scroll_top_row == last_trace_.scroll_top_row &&
        snapshot.viewport_rows == last_trace_.viewport_rows &&
        snapshot.total_rows == last_trace_.total_rows &&
        snapshot.row_begin == last_trace_.row_begin &&
        snapshot.row_end == last_trace_.row_end &&
        snapshot.visible_messages == last_trace_.visible_messages &&
        snapshot.chat_x_min == last_trace_.chat_x_min &&
        snapshot.chat_y_min == last_trace_.chat_y_min &&
        snapshot.chat_x_max == last_trace_.chat_x_max &&
        snapshot.chat_y_max == last_trace_.chat_y_max &&
        snapshot.scrollbar_x_min == last_trace_.scrollbar_x_min &&
        snapshot.scrollbar_y_min == last_trace_.scrollbar_y_min &&
        snapshot.scrollbar_x_max == last_trace_.scrollbar_x_max &&
        snapshot.scrollbar_y_max == last_trace_.scrollbar_y_max &&
        snapshot.cache_misses == last_trace_.cache_misses &&
        snapshot.cache_builds == last_trace_.cache_builds &&
        snapshot.row_count_misses == last_trace_.row_count_misses &&
        snapshot.row_count_builds == last_trace_.row_count_builds &&
        snapshot.cache_size == last_trace_.cache_size;
    if (same_trace) {
        return;
    }
    last_trace_ = snapshot;

    LOG_DEBUG("[chat-viewport] phase=" + std::string(phase) +
              " top=" + std::to_string(state_.scroll_top_row) +
              " viewport_rows=" + std::to_string(state_.viewport_rows) +
              " total_rows=" + std::to_string(state_.total_rows) +
              " visible_rows=" + std::to_string(visible.row_begin) + ".." +
              std::to_string(visible.row_end) +
              " visible_messages=" +
              std::to_string(snapshot.visible_messages) +
              " chat_box=" + format_box(chat_box_) +
              " scrollbar_box=" + format_box(scrollbar_box_) +
              " cache_hits=" + std::to_string(stats.hits) +
              " cache_misses=" + std::to_string(stats.misses) +
              " cache_builds=" + std::to_string(stats.builds) +
              " row_count_hits=" + std::to_string(stats.row_count_hits) +
              " row_count_misses=" + std::to_string(stats.row_count_misses) +
              " row_count_builds=" + std::to_string(stats.row_count_builds) +
              " cache_size=" + std::to_string(snapshot.cache_size));
}

} // namespace acecode::tui
