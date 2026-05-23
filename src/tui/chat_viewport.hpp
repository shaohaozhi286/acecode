#pragma once

#include "tui/chat_viewport_cache.hpp"
#include "tui/chat_viewport_model.hpp"

#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/screen.hpp>

#include <cstddef>
#include <utility>
#include <vector>

namespace acecode::tui {

struct ChatViewportOptions {
    int scrollbar_width = 3;
    int wheel_lines = 3;
    int overscan_rows = 0;
    bool debug_trace = false;
};

class ChatViewport : public ftxui::ComponentBase {
public:
    ChatViewport();
    explicit ChatViewport(ChatViewportOptions options);

    ftxui::Element OnRender() override;
    bool OnEvent(ftxui::Event event) override;

    void set_messages(std::vector<ChatViewportMessageInput> messages);
    void set_options(ChatViewportOptions options);

    int scroll_by_rows(int delta_rows);
    int scroll_to_row(int row);
    int scroll_to_tail();
    bool is_at_tail() const;
    ChatDisplayRow top_display_row() const;
    std::pair<int, int> visible_message_index_bounds() const;

    const ChatViewportState& state() const;
    const ftxui::Box& chat_box() const;
    const ftxui::Box& scrollbar_box() const;
    ChatViewportCacheStats cache_stats() const;

    void refresh_layout_for_box(ftxui::Box box);
    void render_to_screen(ftxui::Screen& screen);

private:
    bool has_chat_box() const;
    bool has_scrollbar_box() const;
    void rebuild_layout_for_box(ftxui::Box box);
    int scrollbar_y_to_row(int mouse_y) const;
    void begin_scrollbar_drag(int mouse_y);
    void move_scrollbar_drag(int mouse_y);
    void end_scrollbar_drag();
    void trace_viewport(const char* phase) const;

    struct TraceSnapshot {
        int scroll_top_row = -1;
        int viewport_rows = -1;
        int total_rows = -1;
        int row_begin = -1;
        int row_end = -1;
        int visible_messages = -1;
        int chat_x_min = -1;
        int chat_y_min = -1;
        int chat_x_max = -1;
        int chat_y_max = -1;
        int scrollbar_x_min = -1;
        int scrollbar_y_min = -1;
        int scrollbar_x_max = -1;
        int scrollbar_y_max = -1;
        int cache_misses = -1;
        int cache_builds = -1;
        int row_count_misses = -1;
        int row_count_builds = -1;
        std::size_t cache_size = static_cast<std::size_t>(-1);
    };

    ChatViewportOptions options_;
    std::vector<ChatViewportMessageInput> messages_;
    ChatViewportLayoutCache cache_;
    ChatViewportState state_;
    std::vector<int> message_row_counts_;
    std::vector<int> drag_row_counts_snapshot_;
    ftxui::Box outer_box_;
    ftxui::Box chat_box_;
    ftxui::Box scrollbar_box_;
    bool layout_dirty_ = true;
    bool scrollbar_dragging_ = false;
    int drag_scrollbar_grab_offset_2x_ = 0;
    mutable TraceSnapshot last_trace_;
};

} // namespace acecode::tui
