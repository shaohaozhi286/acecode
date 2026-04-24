#include "text_input_ops.hpp"

namespace acecode {

namespace {

// UTF-8 continuation byte 判定:10xxxxxx(高两位固定为 10)。这一条和
// main.cpp ArrowLeft / Backspace / Delete / ArrowRight 分支逐字扫描用的
// 判定字节级等价,改动时两边 MUST 同步。
inline bool is_continuation(unsigned char c) {
    return (c & 0xC0) == 0x80;
}

inline void clamp_cursor(const std::string& text, std::size_t& cursor) {
    if (cursor > text.size()) {
        cursor = text.size();
    }
}

} // namespace

void insert_at_cursor(std::string& text, std::size_t& cursor, std::string_view ch) {
    clamp_cursor(text, cursor);
    text.insert(cursor, ch.data(), ch.size());
    cursor += ch.size();
}

void backspace_utf8(std::string& text, std::size_t& cursor) {
    clamp_cursor(text, cursor);
    if (text.empty() || cursor == 0) return;
    std::size_t pos = cursor - 1;
    while (pos > 0 &&
           is_continuation(static_cast<unsigned char>(text[pos]))) {
        pos--;
    }
    text.erase(pos, cursor - pos);
    cursor = pos;
}

void delete_utf8(std::string& text, std::size_t& cursor) {
    clamp_cursor(text, cursor);
    if (cursor >= text.size()) return;
    std::size_t next = cursor + 1;
    while (next < text.size() &&
           is_continuation(static_cast<unsigned char>(text[next]))) {
        next++;
    }
    text.erase(cursor, next - cursor);
}

void move_cursor_left_utf8(const std::string& text, std::size_t& cursor) {
    clamp_cursor(text, cursor);
    if (cursor == 0) return;
    std::size_t pos = cursor - 1;
    while (pos > 0 &&
           is_continuation(static_cast<unsigned char>(text[pos]))) {
        pos--;
    }
    cursor = pos;
}

void move_cursor_right_utf8(const std::string& text, std::size_t& cursor) {
    clamp_cursor(text, cursor);
    if (cursor >= text.size()) return;
    std::size_t next = cursor + 1;
    while (next < text.size() &&
           is_continuation(static_cast<unsigned char>(text[next]))) {
        next++;
    }
    cursor = next;
}

} // namespace acecode
