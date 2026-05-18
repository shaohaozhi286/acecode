#include "slash_dropdown.hpp"
#include "../commands/command_registry.hpp"
#include "picker_scroll.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/string.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace acecode {

namespace {

constexpr int kNarrowTerminalColumns = 40;
constexpr const char* kHorizontalLine = "\xE2\x94\x80";

bool contains_whitespace(const std::string& s) {
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return true;
    }
    return false;
}

std::string repeat_utf8(const char* glyph, int count) {
    std::string out;
    if (count <= 0) return out;
    const std::string g(glyph);
    out.reserve(g.size() * static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) out += g;
    return out;
}

std::string truncate_cells(const std::string& value, int max_cells) {
    if (max_cells <= 0) return {};
    if (ftxui::string_width(value) <= max_cells) return value;
    if (max_cells <= 3) return value.substr(0, static_cast<size_t>(max_cells));

    std::string out;
    int width = 0;
    for (const auto& glyph : ftxui::Utf8ToGlyphs(value)) {
        if (glyph.empty()) continue;
        const int glyph_width = std::max(0, ftxui::string_width(glyph));
        if (width + glyph_width > max_cells - 3) break;
        out += glyph;
        width += glyph_width;
    }
    return out + "...";
}

int score_command(const std::string& query,
                  const std::string& name,
                  const std::string& description) {
    if (query.empty()) return 1; // everything matches, equal weight
    if (name.rfind(query, 0) == 0) return 100; // prefix
    if (name.find(query) != std::string::npos) return 50; // substring on name
    if (description.find(query) != std::string::npos) return 10;
    return 0;
}

} // namespace

void refresh_slash_dropdown(TuiState& state, const CommandRegistry& reg) {
    // Determine whether we're in slash-command position.
    const bool in_command_position =
        !state.input_text.empty() &&
        state.input_text[0] == '/' &&
        !contains_whitespace(state.input_text);

    // Leaving command position clears the Esc-dismissal flag so the next '/'
    // can re-open the dropdown.
    if (!in_command_position) {
        state.slash_dropdown_dismissed_for_input = false;
        state.slash_dropdown_active = false;
        state.slash_dropdown_items.clear();
        state.slash_dropdown_selected = 0;
        state.slash_dropdown_view_offset = 0;
        state.slash_dropdown_total_matches = 0;
        return;
    }

    // Suppress while another overlay owns the UI.
    if (state.resume_picker_active || state.rewind_picker_active ||
        state.confirm_pending || state.ask_pending) {
        state.slash_dropdown_active = false;
        state.slash_dropdown_items.clear();
        state.slash_dropdown_selected = 0;
        state.slash_dropdown_view_offset = 0;
        state.slash_dropdown_total_matches = 0;
        return;
    }

    if (state.slash_dropdown_dismissed_for_input) {
        state.slash_dropdown_active = false;
        state.slash_dropdown_items.clear();
        state.slash_dropdown_selected = 0;
        state.slash_dropdown_view_offset = 0;
        state.slash_dropdown_total_matches = 0;
        return;
    }

    // Build scored candidate list.
    const std::string query = state.input_text.substr(1); // drop leading '/'
    struct Scored {
        int score;
        std::string name;
        std::string description;
    };
    std::vector<Scored> scored;
    scored.reserve(reg.commands().size());
    for (const auto& [name, cmd] : reg.commands()) {
        int s = score_command(query, cmd.name, cmd.description);
        if (s > 0) {
            scored.push_back({s, cmd.name, cmd.description});
        }
    }

    // Stable sort by score desc, then name asc.
    std::sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.name < b.name;
    });

    state.slash_dropdown_total_matches = static_cast<int>(scored.size());

    if (scored.empty()) {
        state.slash_dropdown_active = false;
        state.slash_dropdown_items.clear();
        state.slash_dropdown_selected = 0;
        state.slash_dropdown_view_offset = 0;
        return;
    }

    // Remember previously-selected command name so we can preserve selection
    // across filter updates.
    std::string prev_selected_name;
    if (state.slash_dropdown_active &&
        state.slash_dropdown_selected >= 0 &&
        state.slash_dropdown_selected <
            static_cast<int>(state.slash_dropdown_items.size())) {
        prev_selected_name =
            state.slash_dropdown_items[state.slash_dropdown_selected].name;
    }

    // Keep the full ranked list — viewport scrolling in the renderer handles
    // overflow, so the user can reach commands beyond the first kSlashDropdownVisibleRows
    // via Arrow / PgUp / PgDn / Home / End instead of needing to refine the filter.
    state.slash_dropdown_items.clear();
    state.slash_dropdown_items.reserve(scored.size());
    for (auto& s : scored) {
        state.slash_dropdown_items.push_back(
            {std::move(s.name), std::move(s.description)});
    }

    int new_selected = 0;
    if (!prev_selected_name.empty()) {
        for (int i = 0; i < static_cast<int>(state.slash_dropdown_items.size()); ++i) {
            if (state.slash_dropdown_items[i].name == prev_selected_name) {
                new_selected = i;
                break;
            }
        }
    }
    state.slash_dropdown_selected = new_selected;
    state.slash_dropdown_view_offset = acecode::tui::scroll_to_keep_visible(
        state.slash_dropdown_selected, state.slash_dropdown_view_offset,
        acecode::tui::kSlashDropdownVisibleRows,
        static_cast<int>(state.slash_dropdown_items.size()));
    state.slash_dropdown_active = true;
}

ftxui::Element render_slash_dropdown(const TuiState& state,
                                     bool conhost_compat_layout) {
    using namespace ftxui;
    if (!state.slash_dropdown_active || state.slash_dropdown_items.empty()) {
        return emptyElement();
    }

    const int term_cols = Terminal::Size().dimx;
    const bool narrow = term_cols > 0 && term_cols < kNarrowTerminalColumns;

    const int total = static_cast<int>(state.slash_dropdown_items.size());
    const int visible = std::min(acecode::tui::kSlashDropdownVisibleRows, total);
    const int offset = std::clamp(state.slash_dropdown_view_offset, 0,
                                  std::max(0, total - visible));
    const int items_above = offset;
    const int items_below = std::max(0, total - offset - visible);

    Elements rows;
    std::vector<std::string> compat_rows;
    const int compat_max_cols = std::max(1, term_cols > 4 ? term_cols - 4 : term_cols);
    if (items_above > 0) {
        if (conhost_compat_layout) {
            compat_rows.push_back("  ^ " + std::to_string(items_above) + " more above");
        } else {
            rows.push_back(
                text("  \xE2\x86\x91 " + std::to_string(items_above) + " more above")
                | dim | color(Color::GrayDark));
        }
    }
    for (int i = offset; i < offset + visible; ++i) {
        const auto& item = state.slash_dropdown_items[i];
        const bool selected = (i == state.slash_dropdown_selected);
        Element row;
        if (conhost_compat_layout) {
            std::string line = "  /" + item.name;
            if (!narrow && !item.description.empty()) {
                line += " - " + item.description;
            }
            line = truncate_cells(line, compat_max_cols);
            compat_rows.push_back(line);
            row = text(line);
        } else if (narrow) {
            row = text("  /" + item.name + "  ");
        } else {
            std::string desc = item.description;
            if (desc.size() > 60) {
                size_t cut = 59;
                // Walk back to UTF-8 character boundary.
                while (cut > 0 && (static_cast<unsigned char>(desc[cut]) & 0xC0) == 0x80)
                    --cut;
                desc = desc.substr(0, cut) + "\xE2\x80\xA6"; // ellipsis
            }
            row = hbox({
                text("  /" + item.name + "  "),
                text("\xE2\x80\x94 ") | color(Color::GrayDark),
                text(desc) | color(Color::GrayLight),
            });
        }
        if (selected) {
            row = row | bold | color(Color::White) | bgcolor(Color::RGB(0, 80, 120));
        } else {
            row = row | color(Color::GrayLight);
        }
        rows.push_back(row);
    }
    if (items_below > 0) {
        if (conhost_compat_layout) {
            compat_rows.push_back("  v " + std::to_string(items_below) + " more below");
        } else {
            rows.push_back(
                text("  \xE2\x86\x93 " + std::to_string(items_below) + " more below")
                | dim | color(Color::GrayDark));
        }
    }

    if (conhost_compat_layout) {
        int frame_width = 1;
        for (const auto& line : compat_rows) {
            frame_width = std::max(frame_width, ftxui::string_width(line));
        }
        frame_width = std::min(frame_width, compat_max_cols);

        Elements compat_elements;
        compat_elements.push_back(text(repeat_utf8(kHorizontalLine, frame_width)));
        for (int i = 0; i < static_cast<int>(compat_rows.size()); ++i) {
            const int item_index = offset + i - (items_above > 0 ? 1 : 0);
            const bool selected =
                item_index >= offset &&
                item_index < offset + visible &&
                item_index == state.slash_dropdown_selected;
            Element row = text(truncate_cells(compat_rows[i], frame_width));
            if (selected) {
                row = row | bold | color(Color::White) |
                    bgcolor(Color::RGB(0, 80, 120));
            } else {
                row = row | color(Color::GrayLight);
            }
            compat_elements.push_back(row);
        }
        compat_elements.push_back(text(repeat_utf8(kHorizontalLine, frame_width)));
        return vbox(std::move(compat_elements)) | color(Color::Cyan);
    }

    Element body = vbox(std::move(rows));
    return body | border | color(Color::Cyan);
}

} // namespace acecode
