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

std::string trim_ascii_right(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
    }
    return s;
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

std::string truncate_end(const std::string& s, int max_visual_width) {
    if (visual_width(s) <= max_visual_width) return s;
    if (s.empty()) return s;
    if (max_visual_width <= 1) return kEllipsis;

    return take_head(s, max_visual_width - 1) + kEllipsis;
}

std::string truncate_to_width(const std::string& s, int max_visual_width) {
    return truncate_end(s, max_visual_width);
}

std::vector<std::string> wrap_truncate_end(const std::string& s,
                                           int max_visual_width,
                                           std::size_t max_lines) {
    if (s.empty() || max_lines == 0) return {};
    if (max_visual_width <= 1) return {std::string(kEllipsis)};

    std::vector<size_t> starts;
    starts.reserve(s.size());
    for (size_t pos = 0; pos < s.size();) {
        starts.push_back(pos);
        pos = next_codepoint_end(s, pos);
    }
    starts.push_back(s.size());

    std::vector<std::string> lines;
    lines.reserve(max_lines);
    size_t cp = 0;
    while (cp + 1 < starts.size() && lines.size() < max_lines) {
        const bool last_line = lines.size() + 1 == max_lines;
        const size_t line_start_cp = cp;
        const size_t line_start = starts[line_start_cp];
        const size_t remaining_cp_count = starts.size() - 1 - cp;

        if (static_cast<int>(remaining_cp_count) <= max_visual_width) {
            lines.push_back(trim_ascii_right(s.substr(line_start)));
            return lines;
        }

        if (last_line) {
            lines.push_back(truncate_end(trim_ascii_right(s.substr(line_start)),
                                         max_visual_width));
            return lines;
        }

        size_t end_cp = std::min<std::size_t>(
            line_start_cp + static_cast<std::size_t>(max_visual_width),
            starts.size() - 1);
        size_t break_cp = end_cp;
        for (size_t i = line_start_cp + 1; i < end_cp; ++i) {
            const std::string glyph = s.substr(starts[i], starts[i + 1] - starts[i]);
            if (glyph == " " || glyph == "\t") {
                break_cp = i;
            }
        }

        if (break_cp == line_start_cp) {
            break_cp = end_cp;
        }

        lines.push_back(trim_ascii_right(
            s.substr(line_start, starts[break_cp] - line_start)));

        cp = break_cp;
        while (cp + 1 < starts.size()) {
            const std::string glyph = s.substr(starts[cp],
                                               starts[cp + 1] - starts[cp]);
            if (glyph != " " && glyph != "\t") break;
            ++cp;
        }
    }

    return lines;
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
