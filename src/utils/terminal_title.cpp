#include "terminal_title.hpp"

#include <iostream>
#include <algorithm>

namespace acecode {

namespace {

constexpr size_t kMaxTitleBytes = 256;

size_t utf8_safe_prefix(const std::string& text, size_t max_bytes) {
    const size_t limit = std::min(max_bytes, text.size());
    size_t i = 0;
    size_t last_valid = 0;

    while (i < limit) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        size_t seq_len = 0;

        if ((c & 0x80u) == 0) {
            seq_len = 1;
        } else if ((c & 0xE0u) == 0xC0u) {
            seq_len = 2;
        } else if ((c & 0xF0u) == 0xE0u) {
            seq_len = 3;
        } else if ((c & 0xF8u) == 0xF0u) {
            seq_len = 4;
        } else {
            break;
        }

        if (i + seq_len > limit || i + seq_len > text.size()) break;

        bool valid = true;
        for (size_t j = 1; j < seq_len; ++j) {
            const unsigned char cc = static_cast<unsigned char>(text[i + j]);
            if ((cc & 0xC0u) != 0x80u) { valid = false; break; }
        }
        if (!valid) break;

        i += seq_len;
        last_valid = i;
    }
    return last_valid;
}

} // namespace

void set_terminal_title(std::string_view text) {
    std::string buf;
    buf.reserve(text.size() + 6);
    buf.append("\x1b]2;");
    buf.append(text.data(), text.size());
    buf.append("\x1b\\");
    std::cout.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    std::cout.flush();
}

void clear_terminal_title() {
    set_terminal_title(std::string_view{});
}

bool sanitize_title(std::string& inout, std::string& error_out) {
    error_out.clear();
    for (unsigned char c : inout) {
        // Reject any C0 control byte. OSC 2 is single-line; tabs/newlines are
        // also rejected to keep the rendered title predictable.
        if (c < 0x20 || c == 0x7F) {
            error_out = "invalid control character";
            return false;
        }
    }
    if (inout.size() > kMaxTitleBytes) {
        size_t cut = utf8_safe_prefix(inout, kMaxTitleBytes);
        if (cut == 0) {
            error_out = "invalid encoding";
            return false;
        }
        inout.resize(cut);
        error_out = "truncated";
    }
    return true;
}

} // namespace acecode
