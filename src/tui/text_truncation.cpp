#include "text_truncation.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace acecode::tui {
namespace {

constexpr const char* kEllipsis = "\xE2\x80\xA6";

bool is_utf8_continuation(unsigned char c) {
    return (c & 0xC0) == 0x80;
}

size_t utf8_sequence_length(unsigned char c) {
    if ((c & 0x80) == 0x00) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

size_t next_codepoint_end(const std::string& s, size_t start) {
    if (start >= s.size()) return s.size();
    const size_t len = utf8_sequence_length(static_cast<unsigned char>(s[start]));
    const size_t end = start + len;
    if (end > s.size()) return start + 1;
    for (size_t i = start + 1; i < end; ++i) {
        if (!is_utf8_continuation(static_cast<unsigned char>(s[i]))) {
            return start + 1;
        }
    }
    return end;
}

std::string take_head(const std::string& s, int width) {
    if (width <= 0) return {};

    size_t pos = 0;
    int taken = 0;
    while (pos < s.size() && taken < width) {
        pos = next_codepoint_end(s, pos);
        ++taken;
    }
    return s.substr(0, pos);
}

std::string take_tail(const std::string& s, int width) {
    if (width <= 0) return {};

    std::vector<size_t> starts;
    for (size_t pos = 0; pos < s.size();) {
        starts.push_back(pos);
        pos = next_codepoint_end(s, pos);
    }
    if (static_cast<int>(starts.size()) <= width) return s;

    const size_t start = starts[starts.size() - static_cast<size_t>(width)];
    return s.substr(start);
}

} // namespace

int visual_width(const std::string& s) {
    int n = 0;
    for (unsigned char c : s) {
        if (is_utf8_continuation(c)) continue;
        ++n;
    }
    return n;
}

std::string truncate_middle(const std::string& s, int max_visual_width) {
    if (visual_width(s) <= max_visual_width) return s;
    if (max_visual_width <= 1) return kEllipsis;

    const int budget = max_visual_width - 1;
    const int head_budget = std::max(0, (budget * 4 + 5) / 10);
    const int tail_budget = budget - head_budget;

    return take_head(s, head_budget) + kEllipsis + take_tail(s, tail_budget);
}

std::string truncate_middle_segment(const std::string& prefix,
                                    const std::string& segment,
                                    const std::string& suffix,
                                    int max_visual_width) {
    const int fixed_width = visual_width(prefix) + visual_width(suffix);
    const int segment_width = std::max(1, max_visual_width - fixed_width);
    return prefix + truncate_middle(segment, segment_width) + suffix;
}

} // namespace acecode::tui
