#pragma once

#include <string>
#include <deque>
#include <cstddef>

namespace acecode {

// Strip ANSI escape sequences from a string. Handles CSI (ESC [ ... final-byte)
// and OSC (ESC ] ... BEL | ESC \) sequences. Other control bytes (tab, newline,
// carriage return) are left untouched so the line state machine can process them.
// Terminator bytes for CSI: 0x40..0x7E (@-~). For OSC: BEL (0x07) or ST (ESC\).
inline std::string strip_ansi(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    size_t i = 0;
    const size_t n = input.size();
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(input[i]);
        if (c == 0x1B && i + 1 < n) { // ESC
            unsigned char next = static_cast<unsigned char>(input[i + 1]);
            if (next == '[') {
                // CSI: skip until a byte in 0x40..0x7E
                i += 2;
                while (i < n) {
                    unsigned char b = static_cast<unsigned char>(input[i]);
                    i++;
                    if (b >= 0x40 && b <= 0x7E) break;
                }
                continue;
            }
            if (next == ']') {
                // OSC: skip until BEL or ST (ESC backslash)
                i += 2;
                while (i < n) {
                    unsigned char b = static_cast<unsigned char>(input[i]);
                    if (b == 0x07) { i++; break; }
                    if (b == 0x1B && i + 1 < n && input[i + 1] == '\\') {
                        i += 2;
                        break;
                    }
                    i++;
                }
                continue;
            }
            // Other 2-byte escapes (ESC <single char>): drop both
            i += 2;
            continue;
        }
        out.push_back(input[i]);
        i++;
    }
    return out;
}

// Return the offset of the end of the last complete UTF-8 codepoint in `bytes`.
// Anything from [return_value .. bytes.size()) is a partial codepoint that the
// caller should stash and prepend to the next chunk.
//
// Works by scanning backwards to find a valid start byte, then checking whether
// the trailing sequence is complete.
inline size_t utf8_safe_boundary(const std::string& bytes) {
    const size_t n = bytes.size();
    if (n == 0) return 0;
    // Walk back up to 3 positions (max continuation-byte length for 4-byte codepoint)
    for (size_t back = 0; back < 4 && back < n; ++back) {
        size_t idx = n - 1 - back;
        unsigned char b = static_cast<unsigned char>(bytes[idx]);
        if ((b & 0x80) == 0x00) {
            // ASCII byte: boundary is right after it
            return idx + 1;
        }
        if ((b & 0xC0) == 0xC0) {
            // Start byte: determine expected length
            int expected = 0;
            if ((b & 0xE0) == 0xC0) expected = 2;
            else if ((b & 0xF0) == 0xE0) expected = 3;
            else if ((b & 0xF8) == 0xF0) expected = 4;
            else expected = 1; // invalid start byte; treat as boundary
            if ((int)(n - idx) >= expected) {
                // Full sequence present, boundary is at end
                return n;
            }
            // Incomplete: boundary is right before this start byte
            return idx;
        }
        // Continuation byte: keep walking back
    }
    // No start/ascii byte found in last 4 bytes: all continuations, treat as incomplete
    return 0;
}

// Feed a cleaned chunk (ANSI stripped, UTF-8 safe) through a line state machine.
// Updates `current_line`, `tail_lines` (sliding window of the last `max_tail` lines),
// and increments `total_lines` for every complete line seen.
//
// Behavior:
//   \r  clears current_line (progress-bar overwrite)
//   \n  pushes current_line into tail_lines, increments total_lines
//   other characters accumulate into current_line
inline void feed_line_state(const std::string& chunk,
                            std::string& current_line,
                            std::deque<std::string>& tail_lines,
                            int& total_lines,
                            size_t max_tail = 5) {
    for (char c : chunk) {
        if (c == '\r') {
            current_line.clear();
        } else if (c == '\n') {
            tail_lines.push_back(current_line);
            while (tail_lines.size() > max_tail) tail_lines.pop_front();
            total_lines++;
            current_line.clear();
        } else {
            current_line.push_back(c);
        }
    }
}

} // namespace acecode
