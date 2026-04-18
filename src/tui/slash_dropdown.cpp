#include "slash_dropdown.hpp"
#include "../commands/command_registry.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace acecode {

namespace {

constexpr int kMaxItems = 8;
constexpr int kNarrowTerminalColumns = 40;

bool contains_whitespace(const std::string& s) {
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') return true;
    }
    return false;
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
        state.slash_dropdown_total_matches = 0;
        return;
    }

    // Suppress while another overlay owns the UI.
    if (state.resume_picker_active || state.confirm_pending) {
        state.slash_dropdown_active = false;
        state.slash_dropdown_items.clear();
        state.slash_dropdown_selected = 0;
        state.slash_dropdown_total_matches = 0;
        return;
    }

    if (state.slash_dropdown_dismissed_for_input) {
        state.slash_dropdown_active = false;
        state.slash_dropdown_items.clear();
        state.slash_dropdown_selected = 0;
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

    // Truncate to kMaxItems for display.
    if (static_cast<int>(scored.size()) > kMaxItems) {
        scored.resize(kMaxItems);
    }

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
    state.slash_dropdown_active = true;
}

ftxui::Element render_slash_dropdown(const TuiState& state) {
    using namespace ftxui;
    if (!state.slash_dropdown_active || state.slash_dropdown_items.empty()) {
        return text("");
    }

    const int term_cols = Terminal::Size().dimx;
    const bool narrow = term_cols > 0 && term_cols < kNarrowTerminalColumns;

    Elements rows;
    for (int i = 0; i < static_cast<int>(state.slash_dropdown_items.size()); ++i) {
        const auto& item = state.slash_dropdown_items[i];
        const bool selected = (i == state.slash_dropdown_selected);
        Element row;
        if (narrow) {
            row = text("  /" + item.name + "  ");
        } else {
            std::string desc = item.description;
            if (desc.size() > 60) {
                desc = desc.substr(0, 59) + "\xE2\x80\xA6"; // ellipsis
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

    const int extra =
        state.slash_dropdown_total_matches -
        static_cast<int>(state.slash_dropdown_items.size());
    if (extra > 0) {
        rows.push_back(
            text("  (+" + std::to_string(extra) + " more)")
            | dim | color(Color::GrayDark));
    }

    return vbox(std::move(rows)) | border | color(Color::Cyan);
}

} // namespace acecode
