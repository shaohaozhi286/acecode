#include "configure_picker.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

namespace acecode {

// -----------------------------------------------------------------------------
// Pure helpers — unit-testable, no FTXUI / no stdin
// -----------------------------------------------------------------------------

namespace {

std::string lower(std::string v) {
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return v;
}

bool contains_ci(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    return lower(haystack).find(lower(needle)) != std::string::npos;
}

// Stdout is a TTY? Used by run_ftxui_picker to fall back to the plain flow
// when the user piped input or redirected stdout (CI / scripts).
bool stdout_is_tty() {
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(STDOUT_FILENO) != 0;
#endif
}

// Build a single Menu entry label by delegating to format_picker_row with a
// generous width (FTXUI will clip at terminal width during layout; we only
// guard here against pathologically long metadata).
std::string menu_entry_label(const PickerItem& item) {
    return format_picker_row(item.label, item.secondary, 160);
}

// Compute `filtered_indices` — the subset of `items` whose label or secondary
// contains `query` (case-insensitive). Empty query → all indices.
std::vector<std::size_t> compute_filter(const std::vector<PickerItem>& items,
                                         const std::string& query) {
    std::vector<std::size_t> out;
    out.reserve(items.size());
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (query.empty() ||
            contains_ci(items[i].label, query) ||
            contains_ci(items[i].secondary, query)) {
            out.push_back(i);
        }
    }
    return out;
}

} // namespace

std::string format_picker_row(const std::string& label,
                               const std::string& secondary,
                               std::size_t width) {
    if (secondary.empty()) {
        return label;
    }
    constexpr std::size_t gutter = 2;   // two-space gutter before secondary
    constexpr std::size_t ellipsis = 3; // "..."

    // Label on its own already eats the width → return label unchanged, drop
    // secondary. Spec says we never clip the label.
    if (label.size() + gutter + ellipsis >= width) {
        return label;
    }

    std::size_t available = width - label.size() - gutter;
    if (secondary.size() <= available) {
        return label + "  " + secondary;
    }
    // Truncate secondary to fit and append ellipsis. `available` is guaranteed
    // > ellipsis because of the branch above.
    std::size_t keep = available - ellipsis;
    return label + "  " + secondary.substr(0, keep) + "...";
}

// -----------------------------------------------------------------------------
// Plain-stdin fallback — mirrors pre-change configure_catalog.cpp behavior
// -----------------------------------------------------------------------------

namespace {

void print_page_plain(const std::vector<PickerItem>& items,
                      const std::vector<std::size_t>& filtered,
                      std::size_t page,
                      std::size_t page_size,
                      const PickerOptions& opts) {
    std::size_t pages = (filtered.size() + page_size - 1) / page_size;
    if (pages == 0) pages = 1;
    std::size_t begin = page * page_size;
    std::size_t end = std::min(filtered.size(), begin + page_size);

    std::cout << "\n" << opts.title << " ("
              << (filtered.empty() ? 0 : begin + 1) << "-" << end
              << " of " << filtered.size()
              << ", page " << (page + 1) << "/" << pages << "):\n";
    if (opts.allow_custom) {
        std::cout << "  0. <Custom entry...>\n";
    }
    for (std::size_t i = begin; i < end; ++i) {
        std::cout << "  " << (i + 1) << ". "
                  << menu_entry_label(items[filtered[i]]) << "\n";
    }
    std::cout << "Commands: <number> select"
              << (opts.allow_custom ? " (0 = custom)" : "")
              << ", /<query> filter, n/p next/prev page, q quit\n";
}

PickerResult run_plain_stdin_picker(const std::vector<PickerItem>& items,
                                     const PickerOptions& opts) {
    PickerResult result;
    if (items.empty() && !opts.allow_custom) {
        result.cancelled = true;
        return result;
    }

    std::string query;
    std::vector<std::size_t> filtered = compute_filter(items, query);
    std::size_t page = opts.default_index / std::max<std::size_t>(opts.page_size, 1);

    while (true) {
        std::size_t pages = (filtered.size() + opts.page_size - 1) / opts.page_size;
        if (pages == 0) pages = 1;
        if (page >= pages) page = pages - 1;
        print_page_plain(items, filtered, page, opts.page_size, opts);
        std::cout << "> " << std::flush;

        std::string input;
        if (!std::getline(std::cin, input)) {
            result.cancelled = true;
            return result;
        }
        while (!input.empty() &&
               std::isspace(static_cast<unsigned char>(input.back()))) input.pop_back();
        std::size_t lead = 0;
        while (lead < input.size() &&
               std::isspace(static_cast<unsigned char>(input[lead]))) ++lead;
        input.erase(0, lead);

        if (input.empty()) continue;
        if (input == "q" || input == "Q") {
            result.cancelled = true;
            return result;
        }
        if (input == "n" || input == "N") {
            if (page + 1 < pages) ++page;
            continue;
        }
        if (input == "p" || input == "P") {
            if (page > 0) --page;
            continue;
        }
        if (input.front() == '/') {
            query = input.substr(1);
            filtered = compute_filter(items, query);
            page = 0;
            if (filtered.empty()) {
                std::cout << "No matches for '" << query
                          << "'. Type / to clear filter.\n";
            }
            continue;
        }
        try {
            int n = std::stoi(input);
            if (n == 0 && opts.allow_custom) {
                result.custom = true;
                return result;
            }
            if (n >= 1 && n <= static_cast<int>(filtered.size())) {
                result.index = filtered[n - 1];
                return result;
            }
        } catch (...) {}
        std::cout << "Unrecognised input.\n";
    }
}

// -------------------------------------------------------------------------
// FTXUI picker — interactive path
// -------------------------------------------------------------------------

constexpr int kCustomSyntheticRow = -1; // marker for the synthetic "<Custom>" entry

PickerResult run_ftxui_picker_impl(const std::vector<PickerItem>& items,
                                    const PickerOptions& opts) {
    using namespace ftxui;

    PickerResult result;
    if (items.empty() && !opts.allow_custom) {
        result.cancelled = true;
        return result;
    }

    // Mutable state shared between the event handler and the renderer. All
    // FTXUI callbacks fire on the main loop thread, so no mutex is needed.
    std::string filter_query;
    bool filter_active = false;
    std::vector<std::size_t> filtered = compute_filter(items, filter_query);

    // The Menu entries are a flat vector<string>: optional "<Custom>" row at
    // index 0 when allow_custom, then the filtered items. Recomputed whenever
    // the filter changes.
    std::vector<std::string> menu_entries;
    auto rebuild_menu_entries = [&]() {
        menu_entries.clear();
        if (opts.allow_custom) {
            menu_entries.push_back("<Custom entry...>");
        }
        for (std::size_t idx : filtered) {
            menu_entries.push_back(menu_entry_label(items[idx]));
        }
    };
    rebuild_menu_entries();

    int highlight = 0; // index into menu_entries
    if (!opts.allow_custom || !filtered.empty()) {
        // Initial highlight: map options.default_index (into original items)
        // to its position in menu_entries.
        for (std::size_t i = 0; i < filtered.size(); ++i) {
            if (filtered[i] == opts.default_index) {
                highlight = static_cast<int>(i) + (opts.allow_custom ? 1 : 0);
                break;
            }
        }
    }

    // Digit jump-select buffer — resets after 500ms of inactivity.
    std::uint32_t digit_buffer = 0;
    auto last_digit_ts = std::chrono::steady_clock::now() -
                          std::chrono::seconds(10);

    auto reset_digit_buffer = [&]() {
        digit_buffer = 0;
    };

    auto screen = ScreenInteractive::FitComponent();

    // Menu component holds the selection highlight.
    MenuOption menu_option;
    menu_option.on_enter = [&]() {
        // Commit: translate the highlight back to an original-items index or
        // the custom escape hatch.
        if (menu_entries.empty()) return;
        if (opts.allow_custom && highlight == 0) {
            result.custom = true;
            screen.Exit();
            return;
        }
        std::size_t filtered_pos =
            static_cast<std::size_t>(highlight) - (opts.allow_custom ? 1 : 0);
        if (filtered_pos < filtered.size()) {
            result.index = filtered[filtered_pos];
            screen.Exit();
        }
    };
    auto menu = Menu(&menu_entries, &highlight, menu_option);

    // Filter input is a single-line Input we drive programmatically by
    // appending/deleting characters in the event handler. We do not actually
    // give it focus — keeping all typing in our CatchEvent simplifies the
    // interaction state machine (design D3).

    auto renderer = Renderer(menu, [&]() {
        // Header + menu + status line.
        std::string status;
        {
            std::ostringstream oss;
            std::size_t total = filtered.size();
            std::size_t visible_i = static_cast<std::size_t>(highlight) -
                                     (opts.allow_custom ? 1 : 0);
            if (opts.allow_custom && highlight == 0) {
                oss << "custom/" << (total + 1);
            } else if (total == 0) {
                oss << "0/0";
            } else {
                oss << (visible_i + 1) << "/" << total;
            }
            std::size_t pages = (total + opts.page_size - 1) / opts.page_size;
            if (pages == 0) pages = 1;
            std::size_t page =
                opts.allow_custom && highlight == 0
                    ? 0
                    : (visible_i / opts.page_size);
            oss << "   page " << (page + 1) << "/" << pages;
            if (filter_active || !filter_query.empty()) {
                oss << "   filter: " << filter_query << (filter_active ? "_" : "");
            }
            status = oss.str();
        }

        std::string hint = opts.hint.empty()
            ? std::string("↑↓ move · PgUp/PgDn page · Home/End · Enter select · / filter"
                          + std::string(opts.allow_custom ? " · c custom" : "")
                          + " · Esc cancel")
            : opts.hint;

        return vbox({
                   text(opts.title) | bold,
                   separator(),
                   menu->Render() | vscroll_indicator | frame |
                       size(HEIGHT, LESS_THAN,
                            static_cast<int>(opts.page_size) + 2),
                   separator(),
                   text(status) | dim,
                   text(hint) | dim,
               }) |
               border;
    });

    auto component = CatchEvent(renderer, [&](Event event) -> bool {
        auto now = std::chrono::steady_clock::now();

        // ---------------- Filter-mode input ----------------
        if (filter_active) {
            if (event == Event::Escape) {
                // Exit filter mode, clear filter — do NOT cancel picker.
                filter_active = false;
                filter_query.clear();
                filtered = compute_filter(items, filter_query);
                rebuild_menu_entries();
                highlight = std::min(highlight,
                    static_cast<int>(menu_entries.size()) - 1);
                if (highlight < 0) highlight = 0;
                reset_digit_buffer();
                return true;
            }
            if (event == Event::Return) {
                // Fall through to menu_option.on_enter via Menu's own handler.
                // We handle commit here directly so filter-mode Enter works
                // identically to non-filter Enter.
                menu_option.on_enter();
                return true;
            }
            if (event == Event::Backspace) {
                if (!filter_query.empty()) {
                    filter_query.pop_back();
                    filtered = compute_filter(items, filter_query);
                    rebuild_menu_entries();
                    highlight = std::min(highlight,
                        static_cast<int>(menu_entries.size()) - 1);
                    if (highlight < 0) highlight = 0;
                }
                return true;
            }
            if (event.is_character()) {
                const std::string& c = event.character();
                // Accept any printable byte. FTXUI already filters out
                // control codes, but guard against empty strings just in case.
                if (!c.empty()) {
                    filter_query += c;
                    filtered = compute_filter(items, filter_query);
                    rebuild_menu_entries();
                    highlight = std::min(highlight,
                        static_cast<int>(menu_entries.size()) - 1);
                    if (highlight < 0) highlight = 0;
                }
                return true;
            }
            // Arrow / PageUp / PageDown / Home / End in filter mode: let them
            // fall through to the non-filter handlers below.
        }

        // ---------------- Command-mode keys ----------------
        if (event == Event::Escape) {
            result.cancelled = true;
            screen.Exit();
            return true;
        }
        if (event == Event::Character("q") && !filter_active) {
            result.cancelled = true;
            screen.Exit();
            return true;
        }
        if (event == Event::Character("c") && opts.allow_custom && !filter_active) {
            result.custom = true;
            screen.Exit();
            return true;
        }
        if (event == Event::Character("/") && !filter_active) {
            filter_active = true;
            // Keep current filter_query (usually empty); user may be
            // re-entering filter mode after Esc.
            reset_digit_buffer();
            return true;
        }
        if (event == Event::Return) {
            menu_option.on_enter();
            return true;
        }
        if (event == Event::PageDown) {
            highlight = std::min(
                static_cast<int>(menu_entries.size()) - 1,
                highlight + static_cast<int>(opts.page_size));
            reset_digit_buffer();
            return true;
        }
        if (event == Event::PageUp) {
            highlight =
                std::max(0, highlight - static_cast<int>(opts.page_size));
            reset_digit_buffer();
            return true;
        }
        if (event == Event::Home) {
            highlight = 0;
            reset_digit_buffer();
            return true;
        }
        if (event == Event::End) {
            highlight = static_cast<int>(menu_entries.size()) - 1;
            if (highlight < 0) highlight = 0;
            reset_digit_buffer();
            return true;
        }

        // ---------------- Digit jump-select ----------------
        if (!filter_active && event.is_character()) {
            const std::string& c = event.character();
            if (c.size() == 1 && c[0] >= '0' && c[0] <= '9') {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - last_digit_ts).count();
                if (elapsed > 500) digit_buffer = 0;
                last_digit_ts = now;
                digit_buffer = digit_buffer * 10 + (c[0] - '0');
                if (digit_buffer > 999999) digit_buffer = 999999;

                // Digit 0 with allow_custom triggers the custom entry
                // immediately (so "0" is one-keystroke).
                if (opts.allow_custom && digit_buffer == 0) {
                    result.custom = true;
                    screen.Exit();
                    return true;
                }
                // Jump to entry `digit_buffer` in the filtered list.
                std::size_t target_filtered =
                    static_cast<std::size_t>(digit_buffer == 0 ? 1 : digit_buffer) - 1;
                if (target_filtered >= filtered.size() && !filtered.empty()) {
                    target_filtered = filtered.size() - 1;
                }
                int base = opts.allow_custom ? 1 : 0;
                highlight = base + static_cast<int>(target_filtered);
                if (highlight >= static_cast<int>(menu_entries.size())) {
                    highlight = static_cast<int>(menu_entries.size()) - 1;
                }
                if (highlight < 0) highlight = 0;
                return true;
            }
            // Any non-digit key resets the digit buffer.
            reset_digit_buffer();
        }

        // Let Up/Down fall through to Menu.
        return false;
    });

    screen.Loop(component);
    return result;
}

} // namespace

PickerResult run_ftxui_picker(const std::vector<PickerItem>& items,
                               const PickerOptions& opts) {
    if (!stdout_is_tty()) {
        return run_plain_stdin_picker(items, opts);
    }
    return run_ftxui_picker_impl(items, opts);
}

} // namespace acecode
