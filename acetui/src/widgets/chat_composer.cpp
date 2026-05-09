// acetui/src/widgets/chat_composer.cpp

#include "acetui/widgets/chat_composer.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "acetui/terminal.hpp"
#include "../internal/utf8.hpp"

namespace acetui::widgets {

namespace {

std::string move_to(int row, int col) {
    return "\033[" + std::to_string(row) + ";" + std::to_string(col) + "H";
}
constexpr const char* kEraseInLine = "\033[K";
constexpr const char* kDimOn       = "\033[2m";
constexpr const char* kDimOff      = "\033[22m";

// 输入区每行可写宽度 = viewport 全宽减去左侧 prompt 占用宽度。
int content_width_of(int viewport_width, int prompt_w) {
    int w = viewport_width - prompt_w;
    return w < 1 ? 1 : w;
}

// cursor 在 wrap 段网格里的位置。
struct CursorLoc {
    int seg = 0;  // 段索引,与 utf8_wrap 切出的下标一致
    int col = 0;  // 段内列偏移(0-based,显示列单位)
};

// 沿 buf 走到 cursor_pos(字节偏移),按与 utf8_wrap 完全一致的折行逻辑
// 累计 (seg, col)。cursor 落在 byte == cursor_pos 之前。
CursorLoc locate_cursor(const std::string& buf,
                        size_t cursor_pos,
                        int max_width) {
    CursorLoc loc{0, 0};
    if (max_width < 1) max_width = 1;
    size_t bytes = 0;
    while (bytes < buf.size()) {
        if (bytes == cursor_pos) return loc;
        unsigned char c = static_cast<unsigned char>(buf[bytes]);
        int len = internal::utf8_codepoint_byte_len(c);
        int w   = internal::utf8_codepoint_display_width(c);
        if (loc.col + w > max_width && loc.col > 0) {
            loc.seg += 1;
            loc.col  = 0;
        }
        loc.col  += w;
        bytes    += static_cast<size_t>(len);
    }
    return loc;  // cursor at end of buffer
}

// (seg, col) → buf 的字节偏移。如果 target 落在 buf 末尾外,返回 buf.size()。
size_t byte_pos_for(const std::string& buf,
                    int target_seg,
                    int target_col,
                    int max_width) {
    if (max_width < 1) max_width = 1;
    int seg = 0, col = 0;
    size_t bytes = 0;
    while (bytes < buf.size()) {
        if (seg == target_seg && col >= target_col) return bytes;
        if (seg > target_seg) return bytes;
        unsigned char c = static_cast<unsigned char>(buf[bytes]);
        int len = internal::utf8_codepoint_byte_len(c);
        int w   = internal::utf8_codepoint_display_width(c);
        if (col + w > max_width && col > 0) {
            seg += 1;
            col  = 0;
            continue;  // 不前进 bytes,新段顶部重判
        }
        col   += w;
        bytes += static_cast<size_t>(len);
    }
    return buf.size();
}

// 当前 buffer 切成多少 wrap 段(空 buffer 算 1 段)。
int total_segments(const std::string& buf, int max_width) {
    if (buf.empty()) return 1;
    auto segs = internal::utf8_wrap(buf, max_width);
    if (segs.empty()) return 1;
    return static_cast<int>(segs.size());
}

}  // namespace

int ChatComposer::desired_height(int viewport_width) const {
    if (viewport_width < 1) return 3;

    const int prefix_w = std::max(1, prompt_display_width);
    const int cw       = content_width_of(viewport_width, prefix_w);

    int input_lines = 1;
    if (!buffer_.empty()) {
        int bw      = internal::utf8_display_width(buffer_);
        input_lines = (bw + cw - 1) / cw;
    } else if (!placeholder.empty()) {
        int pw      = internal::utf8_display_width(placeholder);
        input_lines = (pw + cw - 1) / cw;
    }
    if (input_lines < 1)               input_lines = 1;
    if (input_lines > max_input_lines) input_lines = max_input_lines;

    int separator_lines = show_separators ? 2 : 0;
    int footer_section  = footer.empty() ? 0 : (footer_top_padding + 1);

    return input_lines + separator_lines + footer_section;
}

EventResult ChatComposer::on_event(const Event& e, AppContext& ctx) {
    const auto* key = std::get_if<KeyEvent>(&e);
    if (key == nullptr) {
        return EventResult::Continue;
    }

    const int prefix_w = std::max(1, prompt_display_width);
    const int cw       = content_width_of(ctx.viewport.width, prefix_w);

    // ── Enter:提交 ──
    if (key->codepoint == key::kEnter) {
        if (buffer_.empty()) {
            return EventResult::Continue;
        }
        const std::string submitted = std::move(buffer_);
        buffer_.clear();
        cursor_pos_ = 0;
        if (on_submit) {
            on_submit(submitted);
        }
        auto wrapped = internal::utf8_wrap(submitted, ctx.viewport.width);
        ctx.insert_history(wrapped);
        return EventResult::Redraw;
    }

    // ── Backspace:删 cursor 前一个 codepoint ──
    if (key->codepoint == key::kBackspace) {
        if (cursor_pos_ == 0) {
            return EventResult::Continue;
        }
        int len = internal::utf8_codepoint_len_before(buffer_, cursor_pos_);
        if (len <= 0) return EventResult::Continue;
        buffer_.erase(cursor_pos_ - static_cast<size_t>(len),
                      static_cast<size_t>(len));
        cursor_pos_ -= static_cast<size_t>(len);
        return EventResult::Redraw;
    }

    // ── Delete:删 cursor 后一个 codepoint ──
    if (key->codepoint == key::kDelete) {
        if (cursor_pos_ >= buffer_.size()) {
            return EventResult::Continue;
        }
        int len = internal::utf8_codepoint_len_at(buffer_, cursor_pos_);
        if (len <= 0) return EventResult::Continue;
        buffer_.erase(cursor_pos_, static_cast<size_t>(len));
        return EventResult::Redraw;
    }

    // ── Left / Right:跨 codepoint 移动 ──
    if (key->codepoint == key::kLeft) {
        if (cursor_pos_ == 0) return EventResult::Continue;
        int len = internal::utf8_codepoint_len_before(buffer_, cursor_pos_);
        if (len <= 0) return EventResult::Continue;
        cursor_pos_ -= static_cast<size_t>(len);
        return EventResult::Redraw;
    }
    if (key->codepoint == key::kRight) {
        if (cursor_pos_ >= buffer_.size()) return EventResult::Continue;
        int len = internal::utf8_codepoint_len_at(buffer_, cursor_pos_);
        if (len <= 0) return EventResult::Continue;
        cursor_pos_ += static_cast<size_t>(len);
        return EventResult::Redraw;
    }

    // ── Home / End:跳到 buffer 首 / 尾 ──
    if (key->codepoint == key::kHome) {
        if (cursor_pos_ == 0) return EventResult::Continue;
        cursor_pos_ = 0;
        return EventResult::Redraw;
    }
    if (key->codepoint == key::kEnd) {
        if (cursor_pos_ == buffer_.size()) return EventResult::Continue;
        cursor_pos_ = buffer_.size();
        return EventResult::Redraw;
    }

    // ── Up / Down:跨 wrap 段移动 ──
    if (key->codepoint == key::kUp) {
        auto loc = locate_cursor(buffer_, cursor_pos_, cw);
        if (loc.seg <= 0) return EventResult::Continue;
        cursor_pos_ = byte_pos_for(buffer_, loc.seg - 1, loc.col, cw);
        return EventResult::Redraw;
    }
    if (key->codepoint == key::kDown) {
        auto loc       = locate_cursor(buffer_, cursor_pos_, cw);
        int total_segs = total_segments(buffer_, cw);
        if (loc.seg >= total_segs - 1) return EventResult::Continue;
        cursor_pos_ = byte_pos_for(buffer_, loc.seg + 1, loc.col, cw);
        return EventResult::Redraw;
    }

    // ── 可见字符:在 cursor 位置插入 ──
    if (key->codepoint >= 0x20 && key->codepoint != 0x7F &&
        key->codepoint < 0xE000 &&  // 排除我们用作 sentinel 的 PUA 区
        !has_mod(key->mods, Modifier::Ctrl)) {
        std::string utf8;
        internal::utf8_append(utf8, key->codepoint);
        if (utf8.empty()) return EventResult::Continue;
        buffer_.insert(cursor_pos_, utf8);
        cursor_pos_ += utf8.size();
        return EventResult::Redraw;
    }

    return EventResult::Continue;
}

void ChatComposer::render(AppContext& ctx) {
    const Viewport& vp = ctx.viewport;
    if (vp.height < 1 || vp.width < 1) {
        return;
    }

    const int prefix_w = std::max(1, prompt_display_width);
    const int cw       = content_width_of(vp.width, prefix_w);

    // 各区域的绝对屏幕行号(1-based)。
    int row             = vp.top;
    int top_sep_row     = show_separators ? row++ : 0;
    int input_first_row = row;
    int input_last_row  = (footer.empty() ? vp.bottom_row()
                                          : vp.bottom_row() - 1 - footer_top_padding)
                          - (show_separators ? 1 : 0);
    int footer_row      = footer.empty() ? 0 : vp.bottom_row()
                          - (show_separators ? 1 : 0);
    int bot_sep_row     = show_separators ? vp.bottom_row() : 0;

    int input_rows_avail = input_last_row - input_first_row + 1;
    if (input_rows_avail < 1) input_rows_avail = 1;

    bool showing_placeholder = buffer_.empty() && !placeholder.empty();
    const std::string& src   = showing_placeholder ? placeholder : buffer_;

    auto segs = internal::utf8_wrap(src, cw);
    if (segs.empty()) {
        segs.push_back("");
    }

    // 输入区超过可用行时只显示后 N 段(光标永远可见)。
    int total_segs    = static_cast<int>(segs.size());
    int first_visible = std::max(0, total_segs - input_rows_avail);

    // 如果 cursor 落在 first_visible 之上的段,把窗口下沿对齐到 cursor 段。
    if (!showing_placeholder) {
        auto loc = locate_cursor(buffer_, cursor_pos_, cw);
        if (loc.seg < first_visible) {
            first_visible = loc.seg;
        } else if (loc.seg >= first_visible + input_rows_avail) {
            first_visible = loc.seg - input_rows_avail + 1;
        }
        if (first_visible < 0) first_visible = 0;
    }
    int visible_count = std::min(input_rows_avail, total_segs - first_visible);

    std::string out;

    // 上 separator
    if (show_separators) {
        const std::string sep(static_cast<size_t>(vp.width), '-');
        out += move_to(top_sep_row, 1);
        out += kEraseInLine;
        out += sep;
    }

    // 输入区
    for (int i = 0; i < input_rows_avail; ++i) {
        int r = input_first_row + i;
        out += move_to(r, 1);
        out += kEraseInLine;
        if (i < visible_count) {
            int seg_idx = first_visible + i;
            if (seg_idx == 0) {
                out += prompt;
            } else {
                out += std::string(static_cast<size_t>(prefix_w), ' ');
            }
            if (showing_placeholder) out += kDimOn;
            out += segs[seg_idx];
            if (showing_placeholder) out += kDimOff;
        }
    }

    // footer + padding
    if (!footer.empty()) {
        for (int i = 0; i < footer_top_padding; ++i) {
            int r = input_last_row + 1 + i;
            out += move_to(r, 1);
            out += kEraseInLine;
        }
        out += move_to(footer_row, 1);
        out += kEraseInLine;
        out += kDimOn;
        out += footer;
        out += kDimOff;
    }

    // 下 separator
    if (show_separators) {
        const std::string sep(static_cast<size_t>(vp.width), '-');
        out += move_to(bot_sep_row, 1);
        out += kEraseInLine;
        out += sep;
    }

    // ── cursor 落点 ──
    int cursor_row;
    int cursor_col;
    if (showing_placeholder) {
        cursor_row = input_first_row;
        cursor_col = prefix_w + 1;
    } else {
        auto loc        = locate_cursor(buffer_, cursor_pos_, cw);
        int visible_seg = loc.seg - first_visible;
        if (visible_seg < 0)                    visible_seg = 0;
        if (visible_seg >= input_rows_avail)    visible_seg = input_rows_avail - 1;
        cursor_row = input_first_row + visible_seg;
        cursor_col = prefix_w + loc.col + 1;
    }
    out += move_to(cursor_row, cursor_col);

    Terminal::write(out);
}

}  // namespace acetui::widgets
