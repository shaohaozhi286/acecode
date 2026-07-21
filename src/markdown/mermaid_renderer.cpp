/*
 * Terminal Mermaid renderer adapted from xAI Grok Build's
 * crates/codegen/xai-grok-markdown/src/mermaid.rs.
 * Upstream repository revision: ba76b0a683fa52e4e60685017b85905451be17bc.
 *
 * Copyright 2023-2026 SpaceXAI
 * Licensed under the Apache License, Version 2.0. See THIRD-PARTY-NOTICES.
 */

#include "markdown/mermaid_renderer.hpp"

#include <ftxui/screen/string.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace acecode::markdown {
namespace {

constexpr int kMaxLabel = 28;
constexpr int kPadding = 1;
constexpr int kGapX = 3;
constexpr int kGapY = 2;
constexpr int kWrapWidth = 24;
constexpr int kMaxLabelLines = 4;
constexpr std::size_t kMaxNodes = 128;
constexpr std::size_t kMaxEdges = 512;
constexpr std::size_t kMaxGroups = 24;
constexpr std::size_t kMaxGroupDepth = 6;
constexpr std::size_t kMaxCanvasCells = std::size_t{1} << 21;
constexpr std::size_t kMaxSourceBytes = std::size_t{1} << 20;
constexpr std::string_view kTooWideHint =
    "This diagram is too wide to display in the terminal.";

int text_width(std::string_view value) {
    return std::max(0, ftxui::string_width(std::string(value)));
}

std::string trim_left(std::string_view value) {
    std::size_t start = 0;
    while (start < value.size() &&
           std::isspace(static_cast<unsigned char>(value[start]))) {
        ++start;
    }
    return std::string(value.substr(start));
}

std::string trim_right(std::string_view value) {
    std::size_t end = value.size();
    while (end > 0 &&
           std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return std::string(value.substr(0, end));
}

std::string trim(std::string_view value) {
    return trim_right(trim_left(value));
}

std::string ascii_lower(std::string_view value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

std::string ascii_upper(std::string_view value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return out;
}

bool starts_with_ci(std::string_view value, std::string_view prefix) {
    if (value.size() < prefix.size()) return false;
    return ascii_lower(value.substr(0, prefix.size())) == ascii_lower(prefix);
}

bool is_blank(std::string_view value) {
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    });
}

std::vector<std::string> source_lines(std::string_view source) {
    std::vector<std::string> lines;
    std::istringstream input{std::string(source)};
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
    }
    if (lines.empty() && !source.empty()) lines.emplace_back(source);
    return lines;
}

std::vector<std::string> glyphs(std::string_view value) {
    auto values = ftxui::Utf8ToGlyphs(std::string(value));
    // FTXUI inserts an empty cell marker after every full-width glyph. The
    // canvas tracks that continuation cell itself, matching unicode_width in
    // the upstream Rust implementation, so discard FTXUI's marker here.
    values.erase(std::remove(values.begin(), values.end(), std::string()),
                 values.end());
    return values;
}

std::string repeat_glyph(std::string_view glyph, int count) {
    std::string out;
    if (count <= 0) return out;
    out.reserve(glyph.size() * static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) out.append(glyph);
    return out;
}

std::string truncate_columns(std::string_view value, int max_width,
                             bool ellipsis = false) {
    if (max_width <= 0) return {};
    if (text_width(value) <= max_width) return std::string(value);
    const std::string marker = ellipsis && max_width >= 1 ? "…" : "";
    const int available = std::max(0, max_width - text_width(marker));
    std::string out;
    int width = 0;
    for (const auto& glyph : glyphs(value)) {
        const int glyph_width = std::max(1, text_width(glyph));
        if (width + glyph_width > available) break;
        out += glyph;
        width += glyph_width;
    }
    out += marker;
    return out;
}

std::vector<std::string> chunk_columns(std::string_view value, int limit) {
    if (limit < 1) limit = 1;
    if (text_width(value) <= limit) return {std::string(value)};
    std::vector<std::string> out;
    std::string current;
    int current_width = 0;
    for (const auto& glyph : glyphs(value)) {
        const int width = std::max(1, text_width(glyph));
        if (!current.empty() && current_width + width > limit) {
            out.push_back(std::move(current));
            current.clear();
            current_width = 0;
        }
        current += glyph;
        current_width += width;
    }
    if (!current.empty()) out.push_back(std::move(current));
    return out;
}

std::vector<std::string> wrap_words(std::string_view value, int limit) {
    if (limit < 1) limit = 1;
    std::istringstream input{std::string(value)};
    std::vector<std::string> out;
    std::string current;
    std::string word;
    while (input >> word) {
        if (current.empty()) {
            current = word;
        } else if (text_width(current) + 1 + text_width(word) <= limit) {
            current += " " + word;
        } else {
            auto chunks = chunk_columns(current, limit);
            out.insert(out.end(), chunks.begin(), chunks.end());
            current = word;
        }
    }
    if (!current.empty()) {
        auto chunks = chunk_columns(current, limit);
        out.insert(out.end(), chunks.begin(), chunks.end());
    }
    if (out.empty()) out.emplace_back();
    return out;
}

void append_span(MermaidLine& line, std::string text, MermaidRole role,
                 bool italic = false) {
    if (text.empty()) return;
    if (!line.spans.empty() && line.spans.back().role == role &&
        line.spans.back().italic == italic) {
        line.spans.back().text += text;
        return;
    }
    line.spans.push_back({std::move(text), role, italic});
}

enum class Shape { Rect, Round, Diamond, Start, End };
enum class Head {
    None,
    Arrow,
    Circle,
    Cross,
    Triangle,
    DiamondFill,
    DiamondOpen,
};
enum class LineKind { Solid, Dotted, Thick };
enum class Direction { Down, Up, Right, Left };

constexpr std::uint8_t kNorth = 1;
constexpr std::uint8_t kEast = 2;
constexpr std::uint8_t kSouth = 4;
constexpr std::uint8_t kWest = 8;

struct Cell {
    std::string glyph = " ";
    MermaidRole role = MermaidRole::Border;
    std::uint8_t mask = 0;
    LineKind line = LineKind::Solid;
    bool continuation = false;
};

std::string mask_glyph(std::uint8_t mask, LineKind kind) {
    static const std::array<std::string_view, 16> light = {
        " ", "│", "─", "╰", "│", "│", "╭", "├",
        "─", "╯", "─", "┴", "╮", "┤", "┬", "┼",
    };
    static const std::array<std::string_view, 16> thick = {
        " ", "┃", "━", "┗", "┃", "┃", "┏", "┣",
        "━", "┛", "━", "┻", "┓", "┫", "┳", "╋",
    };
    static const std::array<std::string_view, 16> dotted = {
        " ", "┆", "┄", "╰", "┆", "┆", "╭", "├",
        "┄", "╯", "┄", "┴", "╮", "┤", "┬", "┼",
    };
    const auto index = static_cast<std::size_t>(mask & 0x0f);
    if (kind == LineKind::Thick) return std::string(thick[index]);
    if (kind == LineKind::Dotted) return std::string(dotted[index]);
    return std::string(light[index]);
}

class Canvas {
public:
    Canvas(int width, int height)
        : width_(std::max(1, width)), height_(std::max(1, height)) {
        const auto cells = static_cast<std::size_t>(width_) *
                           static_cast<std::size_t>(height_);
        if (cells <= kMaxCanvasCells) cells_.resize(cells);
    }

    bool valid() const { return !cells_.empty(); }
    int width() const { return width_; }
    int height() const { return height_; }

    void put_glyph(int x, int y, std::string glyph, MermaidRole role) {
        if (!inside(x, y) || glyph.empty()) return;
        auto& target = at(x, y);
        if (target.continuation && x > 0) at(x - 1, y) = {};
        const int width = std::max(1, text_width(glyph));
        target = {std::move(glyph), role, 0, LineKind::Solid, false};
        for (int offset = 1; offset < width && inside(x + offset, y); ++offset) {
            at(x + offset, y) = {"", role, 0, LineKind::Solid, true};
        }
    }

    void put_text(int x, int y, std::string_view value, MermaidRole role) {
        int column = x;
        for (const auto& glyph : glyphs(value)) {
            const int width = std::max(1, text_width(glyph));
            if (inside(column, y) && column + width <= width_) {
                put_glyph(column, y, glyph, role);
            }
            column += width;
        }
    }

    void line_cell(int x, int y, std::uint8_t mask, LineKind kind,
                   MermaidRole role = MermaidRole::Edge) {
        if (!inside(x, y)) return;
        auto& cell = at(x, y);
        if (cell.continuation) return;
        if (cell.mask == 0 && cell.glyph != " ") return;
        cell.mask |= mask;
        if (kind == LineKind::Thick ||
            (kind == LineKind::Dotted && cell.line == LineKind::Solid)) {
            cell.line = kind;
        }
        cell.glyph = mask_glyph(cell.mask, cell.line);
        cell.role = role;
    }

    void horizontal(int x1, int x2, int y, LineKind kind,
                    MermaidRole role = MermaidRole::Edge) {
        if (x1 > x2) std::swap(x1, x2);
        for (int x = x1; x <= x2; ++x) {
            std::uint8_t mask = 0;
            if (x > x1) mask |= kWest;
            if (x < x2) mask |= kEast;
            line_cell(x, y, mask == 0 ? (kEast | kWest) : mask, kind, role);
        }
    }

    void vertical(int x, int y1, int y2, LineKind kind,
                  MermaidRole role = MermaidRole::Edge) {
        if (y1 > y2) std::swap(y1, y2);
        for (int y = y1; y <= y2; ++y) {
            std::uint8_t mask = 0;
            if (y > y1) mask |= kNorth;
            if (y < y2) mask |= kSouth;
            line_cell(x, y, mask == 0 ? (kNorth | kSouth) : mask, kind, role);
        }
    }

    void box(int x, int y, int width, int height,
             LineKind kind = LineKind::Solid,
             MermaidRole role = MermaidRole::Border) {
        if (width < 2 || height < 2) return;
        horizontal(x, x + width - 1, y, kind, role);
        horizontal(x, x + width - 1, y + height - 1, kind, role);
        vertical(x, y, y + height - 1, kind, role);
        vertical(x + width - 1, y, y + height - 1, kind, role);
    }

    MermaidArt art(bool fallback = false, bool too_wide = false) const {
        MermaidArt result;
        result.fallback = fallback;
        result.too_wide = too_wide;
        if (!valid()) return result;
        for (int y = 0; y < height_; ++y) {
            int end = width_;
            while (end > 0) {
                const auto& cell = at(end - 1, y);
                if (!cell.continuation && cell.glyph == " " && cell.mask == 0) {
                    --end;
                } else {
                    break;
                }
            }
            MermaidLine line;
            for (int x = 0; x < end; ++x) {
                const auto& cell = at(x, y);
                if (cell.continuation) continue;
                append_span(line, cell.glyph.empty() ? " " : cell.glyph,
                            cell.role);
            }
            result.lines.push_back(std::move(line));
        }
        while (!result.lines.empty() &&
               result.lines.back().plain_text().empty()) {
            result.lines.pop_back();
        }
        return result;
    }

private:
    bool inside(int x, int y) const {
        return x >= 0 && y >= 0 && x < width_ && y < height_ && valid();
    }
    Cell& at(int x, int y) {
        return cells_[static_cast<std::size_t>(y) * width_ + x];
    }
    const Cell& at(int x, int y) const {
        return cells_[static_cast<std::size_t>(y) * width_ + x];
    }

    int width_ = 1;
    int height_ = 1;
    std::vector<Cell> cells_;
};

std::vector<std::string> wrap_label(std::string_view label, int width,
                                    int max_lines = kMaxLabelLines) {
    width = std::clamp(width, 1, kMaxLabel);
    std::vector<std::string> result;
    std::string remaining = trim(label);
    while (!remaining.empty() && static_cast<int>(result.size()) < max_lines) {
        if (text_width(remaining) <= width) {
            result.push_back(remaining);
            remaining.clear();
            break;
        }

        std::string line;
        int line_width = 0;
        int last_break_bytes = -1;
        int consumed_bytes = 0;
        for (const auto& glyph : glyphs(remaining)) {
            const int glyph_width = std::max(1, text_width(glyph));
            if (line_width + glyph_width > width) break;
            line += glyph;
            line_width += glyph_width;
            consumed_bytes += static_cast<int>(glyph.size());
            if (glyph == " " || glyph == "_" || glyph == "-" ||
                glyph == "." || glyph == "/") {
                last_break_bytes = consumed_bytes;
            }
        }
        if (line.empty()) break;
        int take = consumed_bytes;
        if (last_break_bytes > 0 && last_break_bytes < consumed_bytes) {
            take = last_break_bytes;
            line = remaining.substr(0, static_cast<std::size_t>(take));
        }
        result.push_back(trim_right(line));
        remaining = trim_left(remaining.substr(static_cast<std::size_t>(take)));
    }
    if (!remaining.empty() && !result.empty()) {
        result.back() = truncate_columns(result.back(), width - 1) + "…";
    }
    if (result.empty()) result.emplace_back();
    return result;
}

std::string decode_html_entities(std::string_view value) {
    std::string out;
    for (std::size_t i = 0; i < value.size();) {
        if (value[i] != '&') {
            out.push_back(value[i++]);
            continue;
        }
        const auto semi = value.find(';', i + 1);
        if (semi == std::string_view::npos || semi - i > 11) {
            out.push_back(value[i++]);
            continue;
        }
        const auto body = value.substr(i + 1, semi - i - 1);
        std::optional<std::uint32_t> code;
        if (body == "lt") code = '<';
        else if (body == "gt") code = '>';
        else if (body == "amp") code = '&';
        else if (body == "quot") code = '"';
        else if (body == "apos") code = '\'';
        else if (!body.empty() && body.front() == '#') {
            try {
                std::string number(body.substr(1));
                int base = 10;
                if (!number.empty() && (number[0] == 'x' || number[0] == 'X')) {
                    number.erase(number.begin());
                    base = 16;
                }
                code = static_cast<std::uint32_t>(std::stoul(number, nullptr, base));
            } catch (...) {
                code.reset();
            }
        }
        if (!code || *code < 0x20 || *code == 0x7f) {
            out.push_back(value[i++]);
            continue;
        }
        if (*code <= 0x7f) {
            out.push_back(static_cast<char>(*code));
        } else if (*code <= 0x7ff) {
            out.push_back(static_cast<char>(0xc0 | (*code >> 6)));
            out.push_back(static_cast<char>(0x80 | (*code & 0x3f)));
        } else if (*code <= 0xffff) {
            out.push_back(static_cast<char>(0xe0 | (*code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((*code >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (*code & 0x3f)));
        } else if (*code <= 0x10ffff) {
            out.push_back(static_cast<char>(0xf0 | (*code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((*code >> 12) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | ((*code >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (*code & 0x3f)));
        }
        i = semi + 1;
    }
    return out;
}

std::string strip_html_tags(std::string_view value) {
    static const std::unordered_set<std::string> tags = {
        "b", "strong", "i", "em", "u", "s", "strike", "del",
        "ins", "mark", "small", "big", "sub", "sup", "code", "kbd",
        "samp", "var", "tt", "span", "font", "q", "abbr", "cite",
        "pre",
    };
    std::string out;
    for (std::size_t i = 0; i < value.size();) {
        if (value[i] != '<') {
            out.push_back(value[i++]);
            continue;
        }
        const auto close = value.find('>', i + 1);
        if (close == std::string_view::npos) {
            out.push_back(value[i++]);
            continue;
        }
        auto body = trim(value.substr(i + 1, close - i - 1));
        if (!body.empty() && body.front() == '/') body.erase(body.begin());
        const auto stop = body.find_first_of(" /\t");
        const auto tag = ascii_lower(body.substr(0, stop));
        if (tag == "br") out.push_back(' ');
        if (tag == "br" || tags.count(tag)) {
            i = close + 1;
            continue;
        }
        out.append(value.substr(i, close - i + 1));
        i = close + 1;
    }
    return out;
}

std::string strip_markdown(std::string value) {
    for (std::string_view marker : {"**", "__"}) {
        std::size_t at = 0;
        while ((at = value.find(marker, at)) != std::string::npos) {
            value.erase(at, marker.size());
        }
    }
    value.erase(std::remove(value.begin(), value.end(), '`'), value.end());
    std::string out;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if ((c == '*' || c == '_') &&
            !(i > 0 && i + 1 < value.size() &&
              std::isalnum(static_cast<unsigned char>(value[i - 1])) &&
              std::isalnum(static_cast<unsigned char>(value[i + 1])))) {
            continue;
        }
        out.push_back(c);
    }
    return trim(out);
}

std::string clean_label(std::string_view raw) {
    std::string label = trim(strip_html_tags(raw));
    if (label.size() >= 2 &&
        ((label.front() == '"' && label.back() == '"') ||
         (label.front() == '\'' && label.back() == '\''))) {
        label = trim(std::string_view(label).substr(1, label.size() - 2));
    }
    if (label.size() >= 2 && label.front() == '`' && label.back() == '`') {
        label = strip_markdown(label.substr(1, label.size() - 2));
    }
    return decode_html_entities(label);
}

std::string first_word(std::string_view source) {
    std::istringstream input{std::string(source)};
    std::string word;
    input >> word;
    return word.empty() ? "diagram" : word;
}

MermaidArt source_fallback(std::string_view source,
                           std::optional<int> max_width,
                           bool too_wide) {
    const int outer_limit = std::max(8, max_width.value_or(120));
    const int body_limit = std::max(8, outer_limit - 4);
    const std::string full_title = " mermaid: " + first_word(source) + " ";
    const std::string title = truncate_columns(full_title, outer_limit - 2, true);

    std::vector<std::string> body;
    bool seen_nonempty = false;
    for (const auto& raw : source_lines(source)) {
        auto line = trim_right(raw);
        if (!seen_nonempty && line.empty()) continue;
        seen_nonempty = true;
        auto chunks = chunk_columns(line, body_limit);
        body.insert(body.end(), chunks.begin(), chunks.end());
    }
    if (body.empty()) body.emplace_back();

    int content_width = text_width(title);
    for (const auto& line : body) {
        content_width = std::max(content_width, text_width(line));
    }
    content_width = std::min(content_width, body_limit);
    const int inner = content_width + 2;

    MermaidArt art;
    art.fallback = true;
    art.too_wide = too_wide;

    MermaidLine top;
    append_span(top, "╭", MermaidRole::Border);
    append_span(top, title, MermaidRole::Title);
    append_span(top, std::string(std::max(0, inner - text_width(title)), ' ') +
                         "╮",
                MermaidRole::Border);
    art.lines.push_back(std::move(top));

    for (const auto& line : body) {
        MermaidLine row;
        append_span(row, "│ ", MermaidRole::Border);
        append_span(row, line, MermaidRole::NodeText);
        append_span(row,
                    std::string(std::max(0, content_width - text_width(line)), ' ') +
                        " │",
                    MermaidRole::Border);
        art.lines.push_back(std::move(row));
    }

    MermaidLine bottom;
    append_span(bottom, "╰" + repeat_glyph("─", inner) + "╯",
                MermaidRole::Border);
    art.lines.push_back(std::move(bottom));

    if (too_wide) {
        for (const auto& line : wrap_words(kTooWideHint, outer_limit)) {
            MermaidLine hint;
            append_span(hint, line, MermaidRole::Hint, true);
            art.lines.push_back(std::move(hint));
        }
    }
    return art;
}

struct Node {
    std::string label;
    Shape shape = Shape::Rect;
    std::vector<std::vector<std::string>> sections;
    std::string annotation;
};

struct Edge {
    std::size_t from = 0;
    std::size_t to = 0;
    std::optional<std::string> label;
    Head head_to = Head::None;
    Head head_from = Head::None;
    LineKind line = LineKind::Solid;
};

struct Group {
    std::string id;
    std::string label;
    std::optional<std::size_t> parent;
};

struct Graph {
    std::vector<Node> nodes;
    std::vector<Edge> edges;
    std::unordered_map<std::string, std::size_t> index;
    std::vector<Group> groups;
    std::vector<std::optional<std::size_t>> node_group;
    std::optional<std::size_t> current_group;
    bool over_cap = false;
    bool invalid = false;
    Direction direction = Direction::Down;

    std::optional<std::size_t> node_index(
        const std::string& id,
        std::optional<std::string> label = std::nullopt,
        Shape shape = Shape::Rect) {
        const auto found = index.find(id);
        if (found != index.end()) {
            if (label) {
                nodes[found->second].label = std::move(*label);
                nodes[found->second].shape = shape;
            }
            return found->second;
        }
        if (nodes.size() >= kMaxNodes) {
            over_cap = true;
            return std::nullopt;
        }
        const auto position = nodes.size();
        index.emplace(id, position);
        nodes.push_back({label.value_or(id), shape, {}, {}});
        node_group.push_back(current_group);
        return position;
    }

    bool add_edge(Edge edge) {
        if (edges.size() >= kMaxEdges) {
            over_cap = true;
            return false;
        }
        edges.push_back(std::move(edge));
        return true;
    }
};

enum class SequenceHead { None, Arrow, OpenArrow, Cross };
enum class SequenceItemKind { Message, Note, Divider, BlockStart, BlockElse, BlockEnd };
enum class NoteAnchor { Left, Right, Over };

struct SequenceParticipant {
    std::string id;
    std::string label;
};

struct SequenceItem {
    SequenceItemKind kind = SequenceItemKind::Message;
    std::size_t from = 0;
    std::size_t to = 0;
    std::string text;
    SequenceHead head = SequenceHead::Arrow;
    LineKind line = LineKind::Solid;
    NoteAnchor note_anchor = NoteAnchor::Over;
};

struct Sequence {
    std::vector<SequenceParticipant> participants;
    std::unordered_map<std::string, std::size_t> index;
    std::vector<SequenceItem> items;
    bool autonumber = false;
    bool invalid = false;

    std::optional<std::size_t> participant(const std::string& id,
                                           std::optional<std::string> label = std::nullopt) {
        const auto found = index.find(id);
        if (found != index.end()) {
            if (label) participants[found->second].label = std::move(*label);
            return found->second;
        }
        if (participants.size() >= kMaxNodes) {
            invalid = true;
            return std::nullopt;
        }
        const auto position = participants.size();
        index.emplace(id, position);
        participants.push_back({id, label.value_or(id)});
        return position;
    }
};

std::optional<Graph> parse_graph(std::string_view source);
std::optional<Graph> parse_state(std::string_view source);
std::optional<Graph> parse_class(std::string_view source);
std::optional<Graph> parse_er(std::string_view source);
std::optional<Sequence> parse_sequence(std::string_view source);
std::optional<MermaidArt> layout_graph(const Graph& graph,
                                       std::optional<int> max_width);
std::optional<MermaidArt> layout_sequence(const Sequence& sequence,
                                          std::optional<int> max_width);

std::optional<std::string> non_empty(std::string value) {
    if (value.empty()) return std::nullopt;
    return value;
}

Direction parse_direction(std::string_view value) {
    const auto direction = ascii_upper(trim(value));
    if (direction == "LR") return Direction::Right;
    if (direction == "RL") return Direction::Left;
    if (direction == "BT") return Direction::Up;
    return Direction::Down;
}

std::vector<std::string> split_statements(std::string_view source) {
    std::vector<std::string> out;
    for (const auto& raw_line : source_lines(source)) {
        std::string current;
        bool quoted = false;
        for (std::size_t i = 0; i < raw_line.size(); ++i) {
            const char c = raw_line[i];
            if (c == '"') {
                quoted = !quoted;
                current.push_back(c);
                continue;
            }
            if (!quoted && c == '%' && i + 1 < raw_line.size() &&
                raw_line[i + 1] == '%') {
                break;
            }
            if (!quoted && c == ';') {
                auto statement = trim(current);
                if (!statement.empty()) out.push_back(std::move(statement));
                current.clear();
                continue;
            }
            current.push_back(c);
        }
        auto statement = trim(current);
        if (!statement.empty()) out.push_back(std::move(statement));
    }
    return out;
}

bool is_id_byte(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

std::size_t skip_spaces(std::string_view value, std::size_t position) {
    while (position < value.size() &&
           (value[position] == ' ' || value[position] == '\t')) {
        ++position;
    }
    return position;
}

struct ParsedShape {
    Shape shape = Shape::Rect;
    std::optional<std::string> label;
    std::size_t next = 0;
};

ParsedShape read_shape(std::string_view statement, std::size_t start,
                       std::string_view closer, Shape shape) {
    std::string text;
    bool in_quotes = false;
    bool quoted_label = false;
    auto probe = skip_spaces(statement, start);
    if (probe < statement.size() && statement[probe] == '"') quoted_label = true;
    std::size_t i = start;
    while (i < statement.size()) {
        if (quoted_label && statement[i] == '"') {
            in_quotes = !in_quotes;
            text.push_back(statement[i++]);
            continue;
        }
        if (!in_quotes && statement.substr(i, closer.size()) == closer) {
            return {shape, clean_label(text), i + closer.size()};
        }
        text.push_back(statement[i++]);
    }
    return {shape, clean_label(text), statement.size()};
}

std::optional<std::pair<std::size_t, std::size_t>> parse_node(
    std::string_view statement, std::size_t start, Graph& graph) {
    auto i = skip_spaces(statement, start);
    const auto id_start = i;
    while (i < statement.size() && is_id_byte(statement[i])) ++i;
    if (i == id_start) return std::nullopt;
    const std::string id(statement.substr(id_start, i - id_start));

    ParsedShape parsed{Shape::Rect, std::nullopt, i};
    if (i < statement.size()) {
        if (statement[i] == '[') {
            if (statement.substr(i, 2) == "[[") {
                parsed = read_shape(statement, i + 2, "]]", Shape::Rect);
            } else if (statement.substr(i, 2) == "[(") {
                parsed = read_shape(statement, i + 2, ")]", Shape::Round);
            } else {
                parsed = read_shape(statement, i + 1, "]", Shape::Rect);
            }
        } else if (statement[i] == '(') {
            if (statement.substr(i, 2) == "((") {
                parsed = read_shape(statement, i + 2, "))", Shape::Round);
            } else if (statement.substr(i, 2) == "([") {
                parsed = read_shape(statement, i + 2, "])", Shape::Round);
            } else {
                parsed = read_shape(statement, i + 1, ")", Shape::Round);
            }
        } else if (statement[i] == '{') {
            if (statement.substr(i, 2) == "{{") {
                parsed = read_shape(statement, i + 2, "}}", Shape::Diamond);
            } else {
                parsed = read_shape(statement, i + 1, "}", Shape::Diamond);
            }
        } else if (statement[i] == '>') {
            parsed = read_shape(statement, i + 1, "]", Shape::Rect);
        }
    }

    const auto index = graph.node_index(id, parsed.label, parsed.shape);
    if (!index) return std::nullopt;
    return std::make_pair(*index, parsed.next);
}

std::optional<std::pair<std::vector<std::size_t>, std::size_t>>
parse_node_group(std::string_view statement, std::size_t start, Graph& graph) {
    const auto first = parse_node(statement, start, graph);
    if (!first) return std::nullopt;
    std::vector<std::size_t> nodes{first->first};
    auto position = first->second;
    while (true) {
        const auto amp = skip_spaces(statement, position);
        if (amp >= statement.size() || statement[amp] != '&') break;
        const auto next = parse_node(statement, amp + 1, graph);
        if (!next) return std::nullopt;
        nodes.push_back(next->first);
        position = next->second;
    }
    return std::make_pair(std::move(nodes), position);
}

LineKind parse_line_kind(std::string_view op) {
    if (op.find('=') != std::string_view::npos) return LineKind::Thick;
    if (op.find('.') != std::string_view::npos) return LineKind::Dotted;
    return LineKind::Solid;
}

std::optional<std::pair<Head, std::size_t>> trailing_head(
    std::string_view statement, std::size_t position) {
    if (position >= statement.size()) return std::nullopt;
    Head head = Head::None;
    if (statement[position] == 'o') head = Head::Circle;
    else if (statement[position] == 'x') head = Head::Cross;
    else return std::nullopt;
    const auto next = position + 1;
    if (next == statement.size() || statement[next] == ' ' ||
        statement[next] == '\t' || statement[next] == '|' ||
        statement[next] == '&' || statement[next] == ';') {
        return std::make_pair(head, next);
    }
    return std::nullopt;
}

struct ParsedLink {
    Head left = Head::None;
    Head right = Head::None;
    LineKind line = LineKind::Solid;
    std::optional<std::string> label;
    std::size_t next = 0;
};

bool is_link_byte(char c) {
    return c == '-' || c == '.' || c == '=' || c == '<' || c == '>';
}

std::optional<ParsedLink> parse_link(std::string_view statement,
                                     std::size_t start) {
    auto i = skip_spaces(statement, start);
    ParsedLink result;
    if (i + 1 < statement.size() &&
        (statement[i] == 'o' || statement[i] == 'x') &&
        (statement[i + 1] == '-' || statement[i + 1] == '.' ||
         statement[i + 1] == '=')) {
        result.left = statement[i] == 'o' ? Head::Circle : Head::Cross;
        ++i;
    }
    const auto op_start = i;
    while (i < statement.size() && is_link_byte(statement[i])) ++i;
    if (i == op_start) return std::nullopt;
    const auto first_op = statement.substr(op_start, i - op_start);
    if (result.left == Head::None && !first_op.empty() && first_op.front() == '<') {
        result.left = Head::Arrow;
    }
    result.line = parse_line_kind(first_op);
    if (first_op.find('>') != std::string_view::npos) result.right = Head::Arrow;
    if (result.right == Head::None) {
        if (auto head = trailing_head(statement, i)) {
            result.right = head->first;
            i = head->second;
        }
    }

    if (i < statement.size() && statement[i] == '|') {
        const auto end = statement.find('|', i + 1);
        if (end == std::string_view::npos) return std::nullopt;
        result.label = non_empty(clean_label(statement.substr(i + 1, end - i - 1)));
        result.next = end + 1;
        return result;
    }

    if (result.right == Head::None) {
        const auto text_start = skip_spaces(statement, i);
        auto operator_start = text_start;
        while (operator_start < statement.size() &&
               !is_link_byte(statement[operator_start])) {
            ++operator_start;
        }
        if (operator_start > text_start && operator_start < statement.size()) {
            auto operator_end = operator_start;
            while (operator_end < statement.size() &&
                   is_link_byte(statement[operator_end])) {
                ++operator_end;
            }
            const auto second_op = statement.substr(
                operator_start, operator_end - operator_start);
            if (second_op.find('>') != std::string_view::npos) {
                result.right = Head::Arrow;
            } else if (auto head = trailing_head(statement, operator_end)) {
                result.right = head->first;
                operator_end = head->second;
            }
            if (result.line == LineKind::Solid) {
                result.line = parse_line_kind(second_op);
            }
            result.label = non_empty(clean_label(
                statement.substr(text_start, operator_start - text_start)));
            result.next = operator_end;
            return result;
        }
    }

    result.next = i;
    return result;
}

void parse_flow_statement(std::string_view statement, Graph& graph) {
    auto previous = parse_node_group(statement, 0, graph);
    if (!previous) return;
    auto position = previous->second;
    auto from_nodes = std::move(previous->first);
    while (true) {
        position = skip_spaces(statement, position);
        if (position >= statement.size()) break;
        const auto link = parse_link(statement, position);
        if (!link) break;
        position = skip_spaces(statement, link->next);
        auto next = parse_node_group(statement, position, graph);
        if (!next) break;
        for (const auto from : from_nodes) {
            for (const auto to : next->first) {
                Edge edge;
                if (link->left == Head::Arrow && link->right != Head::Arrow) {
                    edge.from = to;
                    edge.to = from;
                    edge.head_to = Head::Arrow;
                    edge.head_from = link->right;
                } else {
                    edge.from = from;
                    edge.to = to;
                    edge.head_to = link->right;
                    edge.head_from = link->left;
                }
                edge.label = link->label;
                edge.line = link->line;
                if (!graph.add_edge(std::move(edge))) return;
            }
        }
        from_nodes = std::move(next->first);
        position = next->second;
    }
}

std::pair<std::string, std::string> parse_subgraph_decl(std::string_view rest) {
    const auto text = trim(rest);
    if (text.size() >= 2 && text.front() == '"') {
        const auto close = text.find('"', 1);
        if (close != std::string::npos) {
            const auto label = text.substr(1, close - 1);
            return {label, decode_html_entities(label)};
        }
    }
    const auto open = text.find('[');
    if (open != std::string::npos && !text.empty() && text.back() == ']') {
        const auto id = trim(text.substr(0, open));
        const auto label = clean_label(
            std::string_view(text).substr(open + 1, text.size() - open - 2));
        if (!id.empty() && !label.empty()) return {id, label};
    }
    return {text, text};
}

std::optional<Graph> parse_graph(std::string_view source) {
    const auto statements = split_statements(source);
    if (statements.empty()) return std::nullopt;
    std::istringstream header(statements.front());
    std::string kind;
    std::string direction;
    header >> kind >> direction;
    kind = ascii_lower(kind);
    if (kind != "graph" && kind != "flowchart") return std::nullopt;

    Graph graph;
    graph.direction = parse_direction(direction.empty() ? "TB" : direction);
    std::vector<std::size_t> group_stack;
    for (std::size_t i = 1; i < statements.size(); ++i) {
        const auto& statement = statements[i];
        std::istringstream words(statement);
        std::string first;
        words >> first;
        const auto lower = ascii_lower(first);
        if (lower == "subgraph") {
            if (graph.groups.size() >= kMaxGroups ||
                group_stack.size() >= kMaxGroupDepth) {
                return std::nullopt;
            }
            auto [id, label] = parse_subgraph_decl(
                std::string_view(statement).substr(first.size()));
            graph.groups.push_back({std::move(id), std::move(label),
                                    group_stack.empty()
                                        ? std::nullopt
                                        : std::optional<std::size_t>(group_stack.back())});
            group_stack.push_back(graph.groups.size() - 1);
            graph.current_group = group_stack.back();
            continue;
        }
        if (lower == "end") {
            if (!group_stack.empty()) group_stack.pop_back();
            graph.current_group = group_stack.empty()
                                      ? std::nullopt
                                      : std::optional<std::size_t>(group_stack.back());
            continue;
        }
        if (lower == "classdef" || lower == "class" || lower == "style" ||
            lower == "linkstyle" || lower == "click" || lower == "direction") {
            continue;
        }
        parse_flow_statement(statement, graph);
        if (graph.over_cap) return std::nullopt;
    }
    if (graph.nodes.empty()) return std::nullopt;
    return graph;
}

std::optional<std::size_t> state_endpoint(Graph& graph, std::string id,
                                          bool source) {
    id = trim(id);
    if (id == "[*]") {
        return graph.node_index(source ? "[*]start" : "[*]end",
                                source ? std::optional<std::string>("●")
                                       : std::optional<std::string>("◉"),
                                source ? Shape::Start : Shape::End);
    }
    return graph.node_index(id, std::nullopt, Shape::Round);
}

bool parse_state_declaration(std::string_view statement, Graph& graph) {
    auto rest = trim(statement.substr(std::string_view("state").size()));
    if (!rest.empty() && rest.back() == '{') rest = trim_right(rest.substr(0, rest.size() - 1));
    if (rest.empty()) return true;
    if (rest.front() == '"') {
        const auto close = rest.find('"', 1);
        if (close == std::string::npos) return false;
        const auto label = decode_html_entities(rest.substr(1, close - 1));
        auto after = trim(std::string_view(rest).substr(close + 1));
        std::string id = label;
        if (starts_with_ci(after, "as ")) id = trim(std::string_view(after).substr(3));
        return graph.node_index(id, label, Shape::Round).has_value();
    }
    Shape shape = Shape::Round;
    auto id = rest;
    const auto stereotype = rest.find("<<");
    if (stereotype != std::string::npos) {
        const auto kind = trim(std::string_view(rest).substr(stereotype + 2));
        if (starts_with_ci(kind, "choice")) shape = Shape::Diamond;
        id = trim(std::string_view(rest).substr(0, stereotype));
    }
    if (id.empty() || id.find_first_of(" \t") != std::string::npos) return false;
    return graph.node_index(id, stereotype == std::string::npos
                                    ? std::nullopt
                                    : std::optional<std::string>(id),
                            shape)
        .has_value();
}

std::string trim_state_endpoint(std::string value) {
    value = trim(value);
    while (!value.empty() && (value.front() == '>' || value.front() == '-')) {
        value.erase(value.begin());
    }
    while (!value.empty() && value.back() == '-') value.pop_back();
    return trim(value);
}

bool parse_state_transition(std::string_view statement, Graph& graph) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        const auto arrow = statement.find("-->", start);
        if (arrow == std::string_view::npos) {
            parts.push_back(trim_state_endpoint(std::string(statement.substr(start))));
            break;
        }
        parts.push_back(trim_state_endpoint(
            std::string(statement.substr(start, arrow - start))));
        start = arrow + 3;
    }
    if (parts.size() < 2 || parts.front().empty()) return false;
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        auto from_id = parts[i];
        auto to_part = parts[i + 1];
        std::optional<std::string> label;
        const auto colon = to_part.find(':');
        if (colon != std::string::npos) {
            label = non_empty(decode_html_entities(trim(
                std::string_view(to_part).substr(colon + 1))));
            to_part = trim_state_endpoint(to_part.substr(0, colon));
        }
        if (from_id.empty() || to_part.empty()) return false;
        auto from = state_endpoint(graph, from_id, true);
        auto to = state_endpoint(graph, to_part, false);
        if (!from || !to) return false;
        if (!graph.add_edge({*from, *to, std::move(label), Head::Arrow,
                             Head::None, LineKind::Solid})) {
            return false;
        }
    }
    return true;
}

std::optional<Graph> parse_state(std::string_view source) {
    const auto statements = split_statements(source);
    if (statements.empty()) return std::nullopt;
    std::istringstream header(statements.front());
    std::string kind;
    header >> kind;
    if (!starts_with_ci(kind, "statediagram")) return std::nullopt;

    Graph graph;
    bool in_note = false;
    for (std::size_t i = 1; i < statements.size(); ++i) {
        const auto& statement = statements[i];
        const auto lower_statement = ascii_lower(statement);
        if (in_note) {
            if (lower_statement == "end note") in_note = false;
            continue;
        }
        std::istringstream words(statement);
        std::string first;
        words >> first;
        const auto lower = ascii_lower(first);
        bool ok = true;
        if (lower == "direction") {
            std::string direction;
            words >> direction;
            graph.direction = parse_direction(direction);
        } else if (lower == "note") {
            if (statement.find(':') == std::string::npos) in_note = true;
        } else if (lower == "state") {
            ok = parse_state_declaration(statement, graph);
        } else if (lower == "classdef" || lower == "class" || lower == "hide" ||
                   lower == "scale" || lower == "}" || lower == "--") {
            continue;
        } else if (statement.find("-->") != std::string::npos) {
            ok = parse_state_transition(statement, graph);
        } else {
            const auto colon = statement.find(':');
            if (colon != std::string::npos) {
                const auto id = trim(statement.substr(0, colon));
                const auto label = decode_html_entities(trim(
                    std::string_view(statement).substr(colon + 1)));
                if (id.empty() || label.empty() ||
                    id.find_first_of(" \t") != std::string::npos) {
                    ok = false;
                } else {
                    auto node = graph.node_index(id, label, Shape::Round);
                    ok = node.has_value();
                }
            } else if (statement.find_first_of(" \t") == std::string::npos) {
                ok = graph.node_index(statement, std::nullopt, Shape::Round).has_value();
            } else {
                ok = false;
            }
        }
        if (!ok || graph.over_cap) return std::nullopt;
    }
    if (graph.nodes.empty()) return std::nullopt;
    return graph;
}

void ensure_class_sections(Node& node) {
    if (node.sections.size() < 2) node.sections.resize(2);
}

std::string display_generics(std::string_view value) {
    std::string out;
    bool open = false;
    for (const char c : value) {
        if (c == '~') {
            out.push_back(open ? '>' : '<');
            open = !open;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

void push_class_member(Node& node, std::string_view raw) {
    ensure_class_sections(node);
    auto member = trim(raw);
    if (starts_with_ci(member, "<<")) {
        const auto close = member.find(">>");
        if (close != std::string::npos) {
            node.annotation = trim(std::string_view(member).substr(2, close - 2));
        }
        return;
    }
    member = decode_html_entities(display_generics(member));
    auto& list = member.find('(') == std::string::npos
                     ? node.sections[0]
                     : node.sections[1];
    if (list.size() < 8) list.push_back(std::move(member));
    else if (list.size() == 8) list.push_back("…");
}

struct ClassRelation {
    std::string from;
    std::string to;
    Head head_from = Head::None;
    Head head_to = Head::None;
    LineKind line = LineKind::Solid;
    std::optional<std::string> label;
};

std::pair<std::string, std::string> strip_cardinality_suffix(std::string value) {
    value = trim_right(value);
    if (!value.empty() && value.back() == '"') {
        const auto quote = value.rfind('"', value.size() - 2);
        if (quote != std::string::npos) {
            return {trim_right(value.substr(0, quote)),
                    value.substr(quote + 1, value.size() - quote - 2)};
        }
    }
    return {value, {}};
}

std::pair<std::string, std::string> strip_cardinality_prefix(std::string value) {
    value = trim_left(value);
    if (!value.empty() && value.front() == '"') {
        const auto quote = value.find('"', 1);
        if (quote != std::string::npos) {
            return {trim_left(value.substr(quote + 1)), value.substr(1, quote - 1)};
        }
    }
    return {value, {}};
}

std::optional<ClassRelation> parse_class_relation(std::string_view statement) {
    struct Operator {
        std::string_view text;
        Head from;
        Head to;
        LineKind line;
    };
    static constexpr std::array<Operator, 14> operators = {{
        {"<|--", Head::Triangle, Head::None, LineKind::Solid},
        {"--|>", Head::None, Head::Triangle, LineKind::Solid},
        {"<|..", Head::Triangle, Head::None, LineKind::Dotted},
        {"..|>", Head::None, Head::Triangle, LineKind::Dotted},
        {"*--", Head::DiamondFill, Head::None, LineKind::Solid},
        {"--*", Head::None, Head::DiamondFill, LineKind::Solid},
        {"o--", Head::DiamondOpen, Head::None, LineKind::Solid},
        {"--o", Head::None, Head::DiamondOpen, LineKind::Solid},
        {"<--", Head::Arrow, Head::None, LineKind::Solid},
        {"-->", Head::None, Head::Arrow, LineKind::Solid},
        {"<..", Head::Arrow, Head::None, LineKind::Dotted},
        {"..>", Head::None, Head::Arrow, LineKind::Dotted},
        {"--", Head::None, Head::None, LineKind::Solid},
        {"..", Head::None, Head::None, LineKind::Dotted},
    }};

    std::size_t best = std::string_view::npos;
    const Operator* selected = nullptr;
    for (const auto& op : operators) {
        auto position = statement.find(op.text);
        while (position != std::string_view::npos) {
            const bool bad_left = op.text.front() == 'o' && position > 0 &&
                                  is_id_byte(statement[position - 1]);
            const auto after = position + op.text.size();
            const bool bad_right = op.text.back() == 'o' &&
                                   after < statement.size() &&
                                   is_id_byte(statement[after]);
            if (!bad_left && !bad_right) break;
            position = statement.find(op.text, position + 1);
        }
        if (position < best) {
            best = position;
            selected = &op;
        }
    }
    if (!selected || best == std::string_view::npos) return std::nullopt;

    auto [lhs, card_from] = strip_cardinality_suffix(
        std::string(statement.substr(0, best)));
    auto [rhs, card_to] = strip_cardinality_prefix(std::string(
        statement.substr(best + selected->text.size())));
    std::optional<std::string> relationship;
    const auto colon = rhs.find(':');
    if (colon != std::string::npos) {
        relationship = non_empty(decode_html_entities(trim(
            std::string_view(rhs).substr(colon + 1))));
        rhs = trim(rhs.substr(0, colon));
    }
    lhs = trim(lhs);
    rhs = trim(rhs);
    if (lhs.empty() || rhs.empty() ||
        lhs.find_first_of(" \t") != std::string::npos ||
        rhs.find_first_of(" \t") != std::string::npos) {
        return std::nullopt;
    }
    std::vector<std::string> labels;
    if (!card_from.empty()) labels.push_back(std::move(card_from));
    if (relationship) labels.push_back(std::move(*relationship));
    if (!card_to.empty()) labels.push_back(std::move(card_to));
    std::string label;
    for (const auto& part : labels) {
        if (!label.empty()) label += ' ';
        label += part;
    }
    return ClassRelation{lhs, rhs, selected->from, selected->to,
                         selected->line, non_empty(std::move(label))};
}

std::optional<Graph> parse_class(std::string_view source) {
    const auto statements = split_statements(source);
    if (statements.empty()) return std::nullopt;
    std::istringstream header(statements.front());
    std::string kind;
    header >> kind;
    if (!starts_with_ci(kind, "classdiagram")) return std::nullopt;

    Graph graph;
    std::optional<std::size_t> current_class;
    for (std::size_t i = 1; i < statements.size(); ++i) {
        const auto& statement = statements[i];
        if (current_class) {
            if (statement == "}") current_class.reset();
            else push_class_member(graph.nodes[*current_class], statement);
            continue;
        }
        std::istringstream words(statement);
        std::string first;
        words >> first;
        const auto lower = ascii_lower(first);
        if (lower == "direction") {
            std::string direction;
            words >> direction;
            graph.direction = parse_direction(direction);
            continue;
        }
        if (lower == "note" || lower == "callback" || lower == "click" ||
            lower == "link" || lower == "style" || lower == "cssclass" ||
            lower == "classdef" || lower == "namespace" || lower == "}") {
            continue;
        }
        if (lower == "class") {
            auto rest = trim(std::string_view(statement).substr(first.size()));
            bool open = !rest.empty() && rest.back() == '{';
            if (open) rest = trim_right(rest.substr(0, rest.size() - 1));
            if (rest.empty() || rest.find_first_of(" \t") != std::string::npos) {
                return std::nullopt;
            }
            auto node = graph.node_index(rest);
            if (!node) return std::nullopt;
            ensure_class_sections(graph.nodes[*node]);
            if (open) current_class = *node;
            continue;
        }
        if (starts_with_ci(statement, "<<")) {
            const auto close = statement.find(">>");
            if (close == std::string::npos) return std::nullopt;
            const auto annotation = trim(
                std::string_view(statement).substr(2, close - 2));
            const auto name = trim(
                std::string_view(statement).substr(close + 2));
            if (name.empty() || name.find_first_of(" \t") != std::string::npos) {
                return std::nullopt;
            }
            auto node = graph.node_index(name);
            if (!node) return std::nullopt;
            ensure_class_sections(graph.nodes[*node]);
            graph.nodes[*node].annotation = annotation;
            continue;
        }
        if (auto relation = parse_class_relation(statement)) {
            auto from = graph.node_index(relation->from);
            auto to = graph.node_index(relation->to);
            if (!from || !to) return std::nullopt;
            ensure_class_sections(graph.nodes[*from]);
            ensure_class_sections(graph.nodes[*to]);
            if (!graph.add_edge({*from, *to, relation->label,
                                 relation->head_to, relation->head_from,
                                 relation->line})) {
                return std::nullopt;
            }
            continue;
        }
        const auto colon = statement.find(':');
        if (colon != std::string::npos) {
            const auto id = trim(statement.substr(0, colon));
            const auto member = trim(
                std::string_view(statement).substr(colon + 1));
            if (id.empty() || member.empty() ||
                id.find_first_of(" \t") != std::string::npos) {
                return std::nullopt;
            }
            auto node = graph.node_index(id);
            if (!node) return std::nullopt;
            push_class_member(graph.nodes[*node], member);
            continue;
        }
        return std::nullopt;
    }
    if (graph.nodes.empty() || current_class) return std::nullopt;
    return graph;
}

std::optional<std::tuple<std::string, std::string, LineKind>> parse_er_operator(
    std::string_view token) {
    if (token.size() != 6) return std::nullopt;
    const auto card = [](std::string_view value) -> std::optional<std::string> {
        if (value == "|o" || value == "o|") return "0..1";
        if (value == "||") return "1";
        if (value == "}o" || value == "o{") return "0..*";
        if (value == "}|" || value == "|{") return "1..*";
        return std::nullopt;
    };
    auto left = card(token.substr(0, 2));
    auto right = card(token.substr(4, 2));
    if (!left || !right) return std::nullopt;
    LineKind line;
    if (token.substr(2, 2) == "--") line = LineKind::Solid;
    else if (token.substr(2, 2) == "..") line = LineKind::Dotted;
    else return std::nullopt;
    return std::make_tuple(*left, *right, line);
}

std::optional<std::size_t> er_entity(Graph& graph, std::string token) {
    token = trim(token);
    const auto open = token.find('[');
    if (open != std::string::npos) {
        if (token.back() != ']') return std::nullopt;
        const auto id = trim(token.substr(0, open));
        const auto label = clean_label(
            std::string_view(token).substr(open + 1, token.size() - open - 2));
        if (id.empty() || label.empty()) return std::nullopt;
        auto node = graph.node_index(id, label, Shape::Rect);
        if (node) ensure_class_sections(graph.nodes[*node]);
        return node;
    }
    auto node = graph.node_index(token);
    if (node) ensure_class_sections(graph.nodes[*node]);
    return node;
}

void push_er_attribute(Node& node, std::string_view raw) {
    ensure_class_sections(node);
    std::istringstream words{std::string(raw)};
    std::string token;
    std::string line;
    while (words >> token) {
        if (!token.empty() && token.front() == '"') break;
        if (!line.empty()) line += ' ';
        line += token;
    }
    if (line.empty()) return;
    line = decode_html_entities(line);
    auto& attributes = node.sections[0];
    if (attributes.size() < 8) attributes.push_back(std::move(line));
    else if (attributes.size() == 8) attributes.push_back("…");
}

std::optional<Graph> parse_er(std::string_view source) {
    const auto statements = split_statements(source);
    if (statements.empty()) return std::nullopt;
    std::istringstream header(statements.front());
    std::string kind;
    header >> kind;
    if (ascii_lower(kind) != "erdiagram") return std::nullopt;

    Graph graph;
    std::optional<std::size_t> current_entity;
    for (std::size_t i = 1; i < statements.size(); ++i) {
        const auto& statement = statements[i];
        if (current_entity) {
            if (statement == "}") current_entity.reset();
            else push_er_attribute(graph.nodes[*current_entity], statement);
            continue;
        }

        std::string relationship = statement;
        std::optional<std::string> relationship_label;
        const auto colon = relationship.find(':');
        if (colon != std::string::npos) {
            relationship_label = clean_label(
                std::string_view(relationship).substr(colon + 1));
            relationship = trim(relationship.substr(0, colon));
        }
        std::istringstream words(relationship);
        std::string lhs;
        std::string op;
        std::string rhs;
        std::string extra;
        words >> lhs >> op >> rhs >> extra;
        if (!lhs.empty() && !op.empty() && !rhs.empty() && extra.empty()) {
            if (auto parsed = parse_er_operator(op)) {
                auto from = er_entity(graph, lhs);
                auto to = er_entity(graph, rhs);
                if (!from || !to) return std::nullopt;
                auto [left, right, line] = *parsed;
                std::string label = left;
                if (relationship_label && !relationship_label->empty()) {
                    label += " " + *relationship_label;
                }
                label += " " + right;
                if (!graph.add_edge({*from, *to, label, Head::None,
                                     Head::None, line})) {
                    return std::nullopt;
                }
                continue;
            }
        }

        auto declaration = trim(statement);
        bool open = !declaration.empty() && declaration.back() == '{';
        if (open) declaration = trim_right(
            declaration.substr(0, declaration.size() - 1));
        if (declaration.empty() ||
            declaration.find_first_of(" \t") != std::string::npos) {
            return std::nullopt;
        }
        auto entity = er_entity(graph, declaration);
        if (!entity) return std::nullopt;
        if (open) current_entity = *entity;
    }
    if (graph.nodes.empty() || current_entity) return std::nullopt;
    return graph;
}

std::optional<std::pair<std::size_t, std::size_t>> parse_note_ids(
    std::string_view ids, Sequence& sequence) {
    const auto comma = ids.find(',');
    auto left = trim(ids.substr(0, comma));
    auto right = comma == std::string_view::npos
                     ? left
                     : trim(ids.substr(comma + 1));
    auto a = sequence.participant(left);
    auto b = sequence.participant(right);
    if (!a || !b) return std::nullopt;
    return std::make_pair(*a, *b);
}

bool parse_sequence_note(std::string_view statement, Sequence& sequence) {
    auto rest = trim(statement.substr(std::string_view("note").size()));
    const auto lower = ascii_lower(rest);
    NoteAnchor anchor = NoteAnchor::Over;
    std::size_t prefix = 0;
    if (starts_with_ci(lower, "over ")) {
        anchor = NoteAnchor::Over;
        prefix = 5;
    } else if (starts_with_ci(lower, "left of ")) {
        anchor = NoteAnchor::Left;
        prefix = 8;
    } else if (starts_with_ci(lower, "right of ")) {
        anchor = NoteAnchor::Right;
        prefix = 9;
    } else {
        return false;
    }
    rest = rest.substr(prefix);
    const auto colon = rest.find(':');
    if (colon == std::string::npos) return false;
    auto ids = parse_note_ids(std::string_view(rest).substr(0, colon), sequence);
    if (!ids) return false;
    SequenceItem item;
    item.kind = SequenceItemKind::Note;
    item.from = std::min(ids->first, ids->second);
    item.to = std::max(ids->first, ids->second);
    item.text = decode_html_entities(trim(
        std::string_view(rest).substr(colon + 1)));
    item.note_anchor = anchor;
    sequence.items.push_back(std::move(item));
    return true;
}

bool parse_sequence_message(std::string_view statement, Sequence& sequence,
                            std::size_t message_number) {
    struct Operator {
        std::string_view text;
        LineKind line;
        SequenceHead head;
    };
    static constexpr std::array<Operator, 8> operators = {{
        {"-->>", LineKind::Dotted, SequenceHead::Arrow},
        {"->>", LineKind::Solid, SequenceHead::Arrow},
        {"--x", LineKind::Dotted, SequenceHead::Cross},
        {"-x", LineKind::Solid, SequenceHead::Cross},
        {"--)", LineKind::Dotted, SequenceHead::OpenArrow},
        {"-)", LineKind::Solid, SequenceHead::OpenArrow},
        {"-->", LineKind::Dotted, SequenceHead::Arrow},
        {"->", LineKind::Solid, SequenceHead::Arrow},
    }};
    std::size_t best = std::string_view::npos;
    const Operator* selected = nullptr;
    for (const auto& op : operators) {
        const auto position = statement.find(op.text);
        if (position < best) {
            best = position;
            selected = &op;
        }
    }
    if (!selected || best == std::string_view::npos) return false;
    const auto from_id = trim(statement.substr(0, best));
    auto rest = trim_left(statement.substr(best + selected->text.size()));
    while (!rest.empty() && (rest.front() == '+' || rest.front() == '-')) {
        rest.erase(rest.begin());
    }
    rest = trim_left(rest);
    const auto colon = rest.find(':');
    const auto to_id = trim(rest.substr(0, colon));
    std::string text;
    if (colon != std::string::npos) {
        text = decode_html_entities(trim(
            std::string_view(rest).substr(colon + 1)));
    }
    if (from_id.empty() || to_id.empty()) return false;
    auto from = sequence.participant(from_id);
    auto to = sequence.participant(to_id);
    if (!from || !to) return false;
    if (sequence.autonumber) {
        text = std::to_string(message_number) + "." +
               (text.empty() ? "" : " " + text);
    }
    sequence.items.push_back({SequenceItemKind::Message, *from, *to,
                              std::move(text), selected->head,
                              selected->line, NoteAnchor::Over});
    return true;
}

std::optional<Sequence> parse_sequence(std::string_view source) {
    const auto statements = split_statements(source);
    if (statements.empty()) return std::nullopt;
    std::istringstream header(statements.front());
    std::string kind;
    header >> kind;
    if (ascii_lower(kind) != "sequencediagram") return std::nullopt;

    Sequence sequence;
    std::vector<bool> blocks;
    std::size_t message_number = 0;
    for (std::size_t i = 1; i < statements.size(); ++i) {
        const auto& statement = statements[i];
        std::istringstream words(statement);
        std::string first;
        words >> first;
        const auto lower = ascii_lower(first);
        if (lower == "participant" || lower == "actor") {
            auto rest = trim(std::string_view(statement).substr(first.size()));
            if (rest.empty()) return std::nullopt;
            const auto lower_rest = ascii_lower(rest);
            const auto alias = lower_rest.find(" as ");
            std::string id = alias == std::string::npos
                                 ? rest
                                 : trim(rest.substr(0, alias));
            std::optional<std::string> label;
            if (alias != std::string::npos) {
                label = clean_label(
                    std::string_view(rest).substr(alias + 4));
            }
            if (!sequence.participant(id, std::move(label))) return std::nullopt;
            continue;
        }
        if (lower == "autonumber") {
            sequence.autonumber = true;
            continue;
        }
        if (lower == "activate" || lower == "deactivate" || lower == "create" ||
            lower == "destroy" || lower == "title" || lower == "acctitle" ||
            lower == "accdescr" || lower == "links" || lower == "link" ||
            lower == "properties") {
            continue;
        }
        if (lower == "note") {
            if (!parse_sequence_note(statement, sequence)) return std::nullopt;
            continue;
        }
        const bool branch = lower == "else" || lower == "and" ||
                            lower == "option";
        const bool block = lower == "loop" || lower == "alt" || lower == "opt" ||
                           lower == "par" || lower == "critical" ||
                           lower == "break";
        if (block || branch) {
            if (branch) {
                if (blocks.empty() || !blocks.back()) continue;
            } else {
                blocks.push_back(true);
            }
            sequence.items.push_back({branch ? SequenceItemKind::BlockElse
                                             : SequenceItemKind::BlockStart,
                                      0, 0, decode_html_entities(statement),
                                      SequenceHead::None, LineKind::Solid,
                                      NoteAnchor::Over});
            continue;
        }
        if (lower == "rect" || lower == "box") {
            blocks.push_back(false);
            continue;
        }
        if (lower == "end") {
            if (!blocks.empty()) {
                const bool visible = blocks.back();
                blocks.pop_back();
                if (visible) {
                    sequence.items.push_back({SequenceItemKind::BlockEnd, 0, 0,
                                              "end", SequenceHead::None,
                                              LineKind::Solid, NoteAnchor::Over});
                }
            }
            continue;
        }
        ++message_number;
        if (!parse_sequence_message(statement, sequence, message_number)) {
            return std::nullopt;
        }
        if (sequence.items.size() > kMaxEdges) return std::nullopt;
    }
    if (sequence.participants.empty() || !blocks.empty()) return std::nullopt;
    return sequence;
}

struct NodeBox {
    int x = 0;
    int y = 0;
    int width = 1;
    int height = 1;
    std::vector<std::string> title;
    std::vector<std::vector<std::string>> sections;
    std::string annotation;

    int left() const { return x; }
    int right() const { return x + width - 1; }
    int top() const { return y; }
    int bottom() const { return y + height - 1; }
    int center_x() const { return x + width / 2; }
    int center_y() const { return y + height / 2; }
};

NodeBox measure_node(const Node& node) {
    NodeBox box;
    if (node.shape == Shape::Start || node.shape == Shape::End) {
        box.title = {node.label};
        return box;
    }

    box.title = wrap_label(node.label, kWrapWidth);
    box.annotation = node.annotation.empty()
                         ? std::string()
                         : "«" + node.annotation + "»";
    int content_width = text_width(box.annotation);
    for (const auto& line : box.title) {
        content_width = std::max(content_width, text_width(line));
    }
    for (const auto& section : node.sections) {
        std::vector<std::string> rendered;
        for (const auto& member : section) {
            auto lines = wrap_label(member, kWrapWidth, 2);
            for (auto& line : lines) {
                content_width = std::max(content_width, text_width(line));
                rendered.push_back(std::move(line));
            }
        }
        box.sections.push_back(std::move(rendered));
    }

    content_width = std::max(1, content_width);
    box.width = content_width + 2 * kPadding + 2;
    if (node.shape == Shape::Diamond) box.width += 2;
    box.height = 2 + static_cast<int>(box.title.size());
    if (!box.annotation.empty()) ++box.height;
    for (const auto& section : box.sections) {
        if (section.empty()) continue;
        box.height += 1 + static_cast<int>(section.size());
    }
    if (node.shape == Shape::Diamond) box.height = std::max(3, box.height);
    return box;
}

void put_centered(Canvas& canvas, int x, int width, int y,
                  std::string_view text, MermaidRole role) {
    const auto clipped = truncate_columns(text, std::max(1, width), true);
    const int left = x + std::max(0, (width - text_width(clipped)) / 2);
    canvas.put_text(left, y, clipped, role);
}

void draw_node(Canvas& canvas, const Node& node, const NodeBox& box) {
    if (node.shape == Shape::Start || node.shape == Shape::End) {
        canvas.put_text(box.x, box.y, node.label, MermaidRole::Border);
        return;
    }
    canvas.box(box.x, box.y, box.width, box.height,
               LineKind::Solid, MermaidRole::Border);
    if (node.shape == Shape::Rect) {
        canvas.put_glyph(box.left(), box.top(), "┌", MermaidRole::Border);
        canvas.put_glyph(box.right(), box.top(), "┐", MermaidRole::Border);
        canvas.put_glyph(box.left(), box.bottom(), "└", MermaidRole::Border);
        canvas.put_glyph(box.right(), box.bottom(), "┘", MermaidRole::Border);
    } else if (node.shape == Shape::Diamond) {
        canvas.put_glyph(box.left(), box.center_y(), "◇", MermaidRole::Border);
        canvas.put_glyph(box.right(), box.center_y(), "◇", MermaidRole::Border);
    }

    int row = box.y + 1;
    if (!box.annotation.empty()) {
        put_centered(canvas, box.x + 1, box.width - 2, row++,
                     box.annotation, MermaidRole::Title);
    }
    for (const auto& line : box.title) {
        put_centered(canvas, box.x + 1, box.width - 2, row++,
                     line, MermaidRole::NodeText);
    }
    for (const auto& section : box.sections) {
        if (section.empty()) continue;
        canvas.horizontal(box.x, box.right(), row, LineKind::Solid,
                          MermaidRole::Border);
        canvas.put_glyph(box.x, row, "├", MermaidRole::Border);
        canvas.put_glyph(box.right(), row, "┤", MermaidRole::Border);
        ++row;
        for (const auto& line : section) {
            canvas.put_text(box.x + 1, row++,
                            truncate_columns(line, box.width - 2, true),
                            MermaidRole::NodeText);
        }
    }
}

struct RankLayout {
    std::vector<int> rank;
    std::vector<bool> forward_edge;
    int count = 1;
};

RankLayout compute_ranks(const Graph& graph) {
    RankLayout result;
    result.rank.assign(graph.nodes.size(), 0);
    result.forward_edge.assign(graph.edges.size(), true);
    std::vector<std::vector<std::size_t>> outgoing(graph.nodes.size());
    for (std::size_t i = 0; i < graph.edges.size(); ++i) {
        if (graph.edges[i].from < outgoing.size()) outgoing[graph.edges[i].from].push_back(i);
    }
    std::vector<int> state(graph.nodes.size(), 0);
    const auto visit = [&](const auto& self, std::size_t node) -> void {
        state[node] = 1;
        for (const auto edge_index : outgoing[node]) {
            const auto target = graph.edges[edge_index].to;
            if (target == node || state[target] == 1) {
                result.forward_edge[edge_index] = false;
                continue;
            }
            if (state[target] == 0) self(self, target);
        }
        state[node] = 2;
    };
    for (std::size_t node = 0; node < graph.nodes.size(); ++node) {
        if (state[node] == 0) visit(visit, node);
    }

    std::vector<int> indegree(graph.nodes.size(), 0);
    for (std::size_t i = 0; i < graph.edges.size(); ++i) {
        if (result.forward_edge[i]) ++indegree[graph.edges[i].to];
    }
    std::deque<std::size_t> queue;
    for (std::size_t i = 0; i < indegree.size(); ++i) {
        if (indegree[i] == 0) queue.push_back(i);
    }
    while (!queue.empty()) {
        const auto node = queue.front();
        queue.pop_front();
        for (const auto edge_index : outgoing[node]) {
            if (!result.forward_edge[edge_index]) continue;
            const auto target = graph.edges[edge_index].to;
            result.rank[target] = std::max(result.rank[target], result.rank[node] + 1);
            if (--indegree[target] == 0) queue.push_back(target);
        }
    }
    result.count = 1 + *std::max_element(result.rank.begin(), result.rank.end());
    return result;
}

std::vector<std::vector<std::size_t>> ordered_rank_rows(
    const Graph& graph, const RankLayout& ranks, bool reverse) {
    std::vector<std::vector<std::size_t>> rows(
        static_cast<std::size_t>(ranks.count));
    for (std::size_t node = 0; node < graph.nodes.size(); ++node) {
        int rank = ranks.rank[node];
        if (reverse) rank = ranks.count - 1 - rank;
        rows[static_cast<std::size_t>(rank)].push_back(node);
    }

    for (std::size_t row = 1; row < rows.size(); ++row) {
        std::unordered_map<std::size_t, int> previous_position;
        for (std::size_t i = 0; i < rows[row - 1].size(); ++i) {
            previous_position[rows[row - 1][i]] = static_cast<int>(i);
        }
        std::stable_sort(rows[row].begin(), rows[row].end(),
                         [&](std::size_t left, std::size_t right) {
            const auto score = [&](std::size_t node) {
                double total = 0;
                int count = 0;
                for (const auto& edge : graph.edges) {
                    std::size_t neighbor = graph.nodes.size();
                    if (edge.to == node) neighbor = edge.from;
                    else if (edge.from == node) neighbor = edge.to;
                    const auto found = previous_position.find(neighbor);
                    if (found != previous_position.end()) {
                        total += found->second;
                        ++count;
                    }
                }
                return count == 0 ? std::numeric_limits<double>::infinity()
                                  : total / count;
            };
            return score(left) < score(right);
        });
    }
    return rows;
}

std::string head_glyph(Head head, int dx, int dy) {
    switch (head) {
        case Head::None: return {};
        case Head::Circle: return "○";
        case Head::Cross: return "×";
        case Head::DiamondFill: return "◆";
        case Head::DiamondOpen: return "◇";
        case Head::Arrow:
            if (std::abs(dx) >= std::abs(dy)) return dx >= 0 ? "▶" : "◀";
            return dy >= 0 ? "▼" : "▲";
        case Head::Triangle:
            if (std::abs(dx) >= std::abs(dy)) return dx >= 0 ? "▷" : "◁";
            return dy >= 0 ? "▽" : "△";
    }
    return {};
}

void draw_head(Canvas& canvas, int x, int y, Head head, int dx, int dy) {
    const auto glyph = head_glyph(head, dx, dy);
    if (!glyph.empty()) canvas.put_glyph(x, y, glyph, MermaidRole::Edge);
}

void place_edge_label(Canvas& canvas, const Edge& edge, int x, int y,
                      int width_hint = kWrapWidth) {
    if (!edge.label || edge.label->empty()) return;
    const auto label = truncate_columns(*edge.label, width_hint, true);
    canvas.put_text(x - text_width(label) / 2, y, label,
                    MermaidRole::EdgeLabel);
}

bool group_contains(const Graph& graph, std::size_t group,
                    std::optional<std::size_t> node_group) {
    while (node_group) {
        if (*node_group == group) return true;
        node_group = graph.groups[*node_group].parent;
    }
    return false;
}

int group_depth(const Graph& graph, std::size_t group) {
    int depth = 0;
    auto parent = graph.groups[group].parent;
    while (parent) {
        ++depth;
        parent = graph.groups[*parent].parent;
    }
    return depth;
}

void draw_groups(Canvas& canvas, const Graph& graph,
                 const std::vector<NodeBox>& boxes) {
    std::vector<std::size_t> order(graph.groups.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](auto left, auto right) {
        return group_depth(graph, left) < group_depth(graph, right);
    });
    int maximum_depth = 0;
    for (const auto group : order) maximum_depth = std::max(maximum_depth, group_depth(graph, group));

    for (const auto group : order) {
        int left = std::numeric_limits<int>::max();
        int top = std::numeric_limits<int>::max();
        int right = -1;
        int bottom = -1;
        for (std::size_t node = 0; node < boxes.size(); ++node) {
            if (!group_contains(graph, group, graph.node_group[node])) continue;
            left = std::min(left, boxes[node].left());
            top = std::min(top, boxes[node].top());
            right = std::max(right, boxes[node].right());
            bottom = std::max(bottom, boxes[node].bottom());
        }
        if (right < left) continue;
        const int margin = 2 + 2 * (maximum_depth - group_depth(graph, group));
        left = std::max(0, left - margin);
        top = std::max(0, top - margin);
        right = std::min(canvas.width() - 1, right + margin);
        bottom = std::min(canvas.height() - 1, bottom + margin);
        canvas.box(left, top, right - left + 1, bottom - top + 1,
                   LineKind::Solid, MermaidRole::Border);
        const auto title = truncate_columns(graph.groups[group].label,
                                             std::max(1, right - left - 3), true);
        canvas.put_text(left + 2, top, " " + title + " ", MermaidRole::Title);
    }
}

void route_vertical_edge(Canvas& canvas, const Edge& edge,
                         const NodeBox& from, const NodeBox& to,
                         std::optional<int> outer_lane) {
    const bool downward = to.center_y() >= from.center_y();
    const int start_x = from.center_x();
    const int start_y = downward ? from.bottom() + 1 : from.top() - 1;
    const int end_x = to.center_x();
    const int end_y = downward ? to.top() - 1 : to.bottom() + 1;

    if (outer_lane) {
        canvas.vertical(start_x, std::min(start_y, from.center_y()),
                        std::max(start_y, from.center_y()), edge.line);
        canvas.horizontal(start_x, *outer_lane, start_y, edge.line);
        canvas.vertical(*outer_lane, start_y, end_y, edge.line);
        canvas.horizontal(*outer_lane, end_x, end_y, edge.line);
        draw_head(canvas, end_x, end_y, edge.head_to,
                  end_x - *outer_lane, end_y - start_y);
        draw_head(canvas, start_x, start_y, edge.head_from,
                  start_x - *outer_lane, start_y - end_y);
        place_edge_label(canvas, edge, *outer_lane, (start_y + end_y) / 2);
        return;
    }

    const int bus = (start_y + end_y) / 2;
    if (start_x == end_x) {
        canvas.vertical(start_x, start_y, end_y, edge.line);
        draw_head(canvas, end_x, end_y, edge.head_to, 0, end_y - start_y);
        draw_head(canvas, start_x, start_y, edge.head_from, 0, start_y - end_y);
        place_edge_label(canvas, edge, start_x, bus);
        return;
    }
    canvas.vertical(start_x, start_y, bus, edge.line);
    canvas.horizontal(start_x, end_x, bus, edge.line);
    canvas.vertical(end_x, bus, end_y, edge.line);
    draw_head(canvas, end_x, end_y, edge.head_to,
              end_x - start_x, end_y - bus);
    draw_head(canvas, start_x, start_y, edge.head_from,
              start_x - end_x, start_y - bus);
    place_edge_label(canvas, edge, (start_x + end_x) / 2, bus);
}

void route_horizontal_edge(Canvas& canvas, const Edge& edge,
                           const NodeBox& from, const NodeBox& to,
                           std::optional<int> outer_lane) {
    const bool rightward = to.center_x() >= from.center_x();
    const int start_x = rightward ? from.right() + 1 : from.left() - 1;
    const int start_y = from.center_y();
    const int end_x = rightward ? to.left() - 1 : to.right() + 1;
    const int end_y = to.center_y();

    if (outer_lane) {
        canvas.horizontal(start_x, from.center_x(), start_y, edge.line);
        canvas.vertical(start_x, start_y, *outer_lane, edge.line);
        canvas.horizontal(start_x, end_x, *outer_lane, edge.line);
        canvas.vertical(end_x, *outer_lane, end_y, edge.line);
        draw_head(canvas, end_x, end_y, edge.head_to,
                  end_x - start_x, end_y - *outer_lane);
        draw_head(canvas, start_x, start_y, edge.head_from,
                  start_x - end_x, start_y - *outer_lane);
        place_edge_label(canvas, edge, (start_x + end_x) / 2, *outer_lane);
        return;
    }

    const int bus = (start_x + end_x) / 2;
    if (start_y == end_y) {
        canvas.horizontal(start_x, end_x, start_y, edge.line);
        draw_head(canvas, end_x, end_y, edge.head_to, end_x - start_x, 0);
        draw_head(canvas, start_x, start_y, edge.head_from, start_x - end_x, 0);
        place_edge_label(canvas, edge, bus, start_y);
        return;
    }
    canvas.horizontal(start_x, bus, start_y, edge.line);
    canvas.vertical(bus, start_y, end_y, edge.line);
    canvas.horizontal(bus, end_x, end_y, edge.line);
    draw_head(canvas, end_x, end_y, edge.head_to,
              end_x - bus, end_y - start_y);
    draw_head(canvas, start_x, start_y, edge.head_from,
              start_x - bus, start_y - end_y);
    place_edge_label(canvas, edge, bus, std::min(start_y, end_y));
}

void route_self_edge(Canvas& canvas, const Edge& edge, const NodeBox& box,
                     bool horizontal_layout) {
    if (!horizontal_layout) {
        const int x = box.right() + 2;
        const int y1 = box.center_y();
        const int y2 = std::min(canvas.height() - 1, box.bottom() + 2);
        canvas.horizontal(box.right() + 1, x, y1, edge.line);
        canvas.vertical(x, y1, y2, edge.line);
        canvas.horizontal(box.center_x(), x, y2, edge.line);
        draw_head(canvas, box.center_x(), y2, edge.head_to, -1, 0);
        place_edge_label(canvas, edge, x, y1 + 1);
    } else {
        const int y = box.bottom() + 2;
        const int x1 = box.center_x();
        const int x2 = std::min(canvas.width() - 1, box.right() + 2);
        canvas.vertical(x1, box.bottom() + 1, y, edge.line);
        canvas.horizontal(x1, x2, y, edge.line);
        canvas.vertical(x2, box.center_y(), y, edge.line);
        draw_head(canvas, x2, box.center_y(), edge.head_to, 0, -1);
        place_edge_label(canvas, edge, (x1 + x2) / 2, y);
    }
}

std::optional<MermaidArt> layout_graph(const Graph& graph,
                                       std::optional<int> max_width) {
    (void)max_width;
    if (graph.nodes.empty()) return std::nullopt;
    const auto ranks = compute_ranks(graph);
    const bool horizontal = graph.direction == Direction::Right ||
                            graph.direction == Direction::Left;
    const bool reverse = graph.direction == Direction::Up ||
                         graph.direction == Direction::Left;
    auto rows = ordered_rank_rows(graph, ranks, reverse);

    std::vector<NodeBox> boxes;
    boxes.reserve(graph.nodes.size());
    for (const auto& node : graph.nodes) boxes.push_back(measure_node(node));

    int maximum_group_depth = 0;
    for (std::size_t group = 0; group < graph.groups.size(); ++group) {
        maximum_group_depth = std::max(maximum_group_depth, group_depth(graph, group));
    }
    const int margin = graph.groups.empty() ? 2 : 5 + 2 * maximum_group_depth;
    int base_width = 1;
    int base_height = 1;

    if (!horizontal) {
        std::vector<int> row_width(rows.size(), 0);
        std::vector<int> row_height(rows.size(), 1);
        for (std::size_t row = 0; row < rows.size(); ++row) {
            for (std::size_t i = 0; i < rows[row].size(); ++i) {
                const auto& box = boxes[rows[row][i]];
                row_width[row] += box.width;
                if (i + 1 < rows[row].size()) row_width[row] += kGapX;
                row_height[row] = std::max(row_height[row], box.height);
            }
            base_width = std::max(base_width, row_width[row]);
        }
        int y = margin;
        for (std::size_t row = 0; row < rows.size(); ++row) {
            int x = margin + (base_width - row_width[row]) / 2;
            for (const auto node : rows[row]) {
                boxes[node].x = x;
                boxes[node].y = y + (row_height[row] - boxes[node].height) / 2;
                x += boxes[node].width + kGapX;
            }
            y += row_height[row] + kGapY + 2;
        }
        base_height = y - kGapY - 2 + margin;
        base_width += 2 * margin;
    } else {
        std::vector<int> column_width(rows.size(), 1);
        std::vector<int> column_height(rows.size(), 0);
        for (std::size_t column = 0; column < rows.size(); ++column) {
            for (std::size_t i = 0; i < rows[column].size(); ++i) {
                const auto& box = boxes[rows[column][i]];
                column_width[column] = std::max(column_width[column], box.width);
                column_height[column] += box.height;
                if (i + 1 < rows[column].size()) column_height[column] += kGapY;
            }
            base_height = std::max(base_height, column_height[column]);
        }
        int x = margin;
        for (std::size_t column = 0; column < rows.size(); ++column) {
            int y = margin + (base_height - column_height[column]) / 2;
            for (const auto node : rows[column]) {
                boxes[node].x = x + (column_width[column] - boxes[node].width) / 2;
                boxes[node].y = y;
                y += boxes[node].height + kGapY;
            }
            x += column_width[column] + kGapX + 2;
        }
        base_width = x - kGapX - 2 + margin;
        base_height += 2 * margin;
    }

    int back_edges = 0;
    for (std::size_t i = 0; i < ranks.forward_edge.size(); ++i) {
        if (!ranks.forward_edge[i] || graph.edges[i].from == graph.edges[i].to) ++back_edges;
    }
    const int width = base_width + (!horizontal ? back_edges * 2 + 2 : 0);
    const int height = base_height + (horizontal ? back_edges * 2 + 2 : 0);
    Canvas canvas(width, height);
    if (!canvas.valid()) return std::nullopt;

    draw_groups(canvas, graph, boxes);
    int lane = 0;
    for (std::size_t i = 0; i < graph.edges.size(); ++i) {
        const auto& edge = graph.edges[i];
        const auto& from = boxes[edge.from];
        const auto& to = boxes[edge.to];
        if (edge.from == edge.to) {
            route_self_edge(canvas, edge, from, horizontal);
            ++lane;
        } else if (horizontal) {
            std::optional<int> outer;
            if (!ranks.forward_edge[i]) outer = base_height + 1 + lane++ * 2;
            route_horizontal_edge(canvas, edge, from, to, outer);
        } else {
            std::optional<int> outer;
            if (!ranks.forward_edge[i]) outer = base_width + 1 + lane++ * 2;
            route_vertical_edge(canvas, edge, from, to, outer);
        }
    }
    for (std::size_t i = 0; i < graph.nodes.size(); ++i) {
        draw_node(canvas, graph.nodes[i], boxes[i]);
    }
    return canvas.art();
}

struct SequenceGeometry {
    std::vector<int> centers;
    std::vector<int> widths;
    int left_margin = 2;
    int width = 1;
    int height = 1;
};

SequenceGeometry measure_sequence(const Sequence& sequence) {
    SequenceGeometry geometry;
    int largest_note = 0;
    for (const auto& item : sequence.items) {
        if (item.kind == SequenceItemKind::Note) {
            largest_note = std::max(largest_note,
                                    std::min(kMaxLabel, text_width(item.text)) + 4);
        }
    }
    geometry.left_margin = std::max(2, largest_note + 2);
    int x = geometry.left_margin;
    for (const auto& participant : sequence.participants) {
        const int width = std::max(5, std::min(kMaxLabel,
                                               text_width(participant.label)) + 4);
        geometry.widths.push_back(width);
        geometry.centers.push_back(x + width / 2);
        x += width + 7;
    }
    geometry.width = x + largest_note + 2;
    int height = 6;
    for (const auto& item : sequence.items) {
        switch (item.kind) {
            case SequenceItemKind::Message:
                height += item.from == item.to ? 4 : 3;
                break;
            case SequenceItemKind::Note: height += 4; break;
            case SequenceItemKind::Divider:
            case SequenceItemKind::BlockStart:
            case SequenceItemKind::BlockElse:
            case SequenceItemKind::BlockEnd: height += 2; break;
        }
    }
    geometry.height = height + 3;
    return geometry;
}

void draw_sequence_actor(Canvas& canvas, int center, int y, int width,
                         std::string_view label) {
    const int left = center - width / 2;
    canvas.box(left, y, width, 3, LineKind::Solid, MermaidRole::Border);
    canvas.put_glyph(left, y, "┌", MermaidRole::Border);
    canvas.put_glyph(left + width - 1, y, "┐", MermaidRole::Border);
    canvas.put_glyph(left, y + 2, "└", MermaidRole::Border);
    canvas.put_glyph(left + width - 1, y + 2, "┘", MermaidRole::Border);
    put_centered(canvas, left + 1, width - 2, y + 1,
                 label, MermaidRole::NodeText);
}

void draw_sequence_note(Canvas& canvas, const SequenceItem& item,
                        const SequenceGeometry& geometry, int y) {
    const auto lines = wrap_label(item.text, kWrapWidth, 2);
    int width = 4;
    for (const auto& line : lines) width = std::max(width, text_width(line) + 4);
    int left = 0;
    if (item.note_anchor == NoteAnchor::Over) {
        const int center = (geometry.centers[item.from] + geometry.centers[item.to]) / 2;
        width = std::max(width,
                         geometry.centers[item.to] - geometry.centers[item.from] + 5);
        left = center - width / 2;
    } else if (item.note_anchor == NoteAnchor::Left) {
        left = geometry.centers[item.from] - width - 2;
    } else {
        left = geometry.centers[item.from] + 2;
    }
    left = std::max(0, std::min(left, canvas.width() - width));
    canvas.box(left, y, width, static_cast<int>(lines.size()) + 2,
               LineKind::Solid, MermaidRole::Border);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        put_centered(canvas, left + 1, width - 2, y + 1 + static_cast<int>(i),
                     lines[i], MermaidRole::NodeText);
    }
}

std::optional<MermaidArt> layout_sequence(const Sequence& sequence,
                                          std::optional<int> max_width) {
    (void)max_width;
    if (sequence.participants.empty()) return std::nullopt;
    const auto geometry = measure_sequence(sequence);
    Canvas canvas(geometry.width, geometry.height);
    if (!canvas.valid()) return std::nullopt;

    const int top = 0;
    const int lifeline_start = 3;
    const int bottom = geometry.height - 3;
    for (std::size_t i = 0; i < sequence.participants.size(); ++i) {
        draw_sequence_actor(canvas, geometry.centers[i], top,
                            geometry.widths[i], sequence.participants[i].label);
        canvas.vertical(geometry.centers[i], lifeline_start, bottom,
                        LineKind::Dotted, MermaidRole::Edge);
        draw_sequence_actor(canvas, geometry.centers[i], bottom,
                            geometry.widths[i], sequence.participants[i].label);
    }

    int y = 4;
    for (const auto& item : sequence.items) {
        if (item.kind == SequenceItemKind::Message) {
            const int from = geometry.centers[item.from];
            const int to = geometry.centers[item.to];
            if (item.from == item.to) {
                const int loop_x = std::min(canvas.width() - 2, from + 5);
                if (!item.text.empty()) {
                    canvas.put_text(from + 1, y, truncate_columns(item.text, kWrapWidth, true),
                                    MermaidRole::EdgeLabel);
                }
                ++y;
                canvas.horizontal(from, loop_x, y, item.line);
                canvas.vertical(loop_x, y, y + 2, item.line);
                canvas.horizontal(from, loop_x, y + 2, item.line);
                const auto head = item.head == SequenceHead::Cross ? "×" : "◀";
                canvas.put_glyph(from, y + 2, head, MermaidRole::Edge);
                y += 3;
            } else {
                if (!item.text.empty()) {
                    put_centered(canvas, std::min(from, to), std::abs(to - from), y,
                                 item.text, MermaidRole::EdgeLabel);
                }
                ++y;
                canvas.horizontal(from, to, y, item.line);
                std::string head;
                if (item.head == SequenceHead::Cross) head = "×";
                else if (item.head == SequenceHead::OpenArrow) head = to > from ? "▷" : "◁";
                else head = to > from ? "▶" : "◀";
                canvas.put_glyph(to, y, head, MermaidRole::Edge);
                y += 2;
            }
            continue;
        }
        if (item.kind == SequenceItemKind::Note) {
            draw_sequence_note(canvas, item, geometry, y);
            y += 4;
            continue;
        }

        const int left = geometry.centers.front();
        const int right = geometry.centers.back();
        canvas.horizontal(left, right, y, LineKind::Solid, MermaidRole::Border);
        std::string label = item.text;
        if (item.kind == SequenceItemKind::BlockEnd) label = "end";
        canvas.put_text(left + 1, y, " " + truncate_columns(label, right - left - 3, true) + " ",
                        MermaidRole::Title);
        y += 2;
    }
    return canvas.art();
}

} // namespace

std::string MermaidLine::plain_text() const {
    std::string out;
    for (const auto& span : spans) out += span.text;
    return out;
}

int MermaidLine::display_width() const {
    return text_width(plain_text());
}

std::optional<MermaidArt> render_mermaid_terminal(
    std::string_view source,
    std::optional<int> max_width) {
    if (is_blank(source)) return std::nullopt;
    if (source.size() > kMaxSourceBytes) {
        return source_fallback(source, max_width, false);
    }
    if (max_width) *max_width = std::max(8, *max_width);

    std::optional<MermaidArt> art;
    if (auto graph = parse_graph(source)) {
        art = layout_graph(*graph, max_width);
    } else if (auto state = parse_state(source)) {
        art = layout_graph(*state, max_width);
    } else if (auto cls = parse_class(source)) {
        art = layout_graph(*cls, max_width);
    } else if (auto er = parse_er(source)) {
        art = layout_graph(*er, max_width);
    } else if (auto sequence = parse_sequence(source)) {
        art = layout_sequence(*sequence, max_width);
    }

    if (!art) return source_fallback(source, max_width, false);
    if (max_width) {
        const bool too_wide = std::any_of(
            art->lines.begin(), art->lines.end(),
            [&](const MermaidLine& line) {
                return line.display_width() > *max_width;
            });
        if (too_wide) return source_fallback(source, max_width, true);
    }
    return art;
}

} // namespace acecode::markdown
