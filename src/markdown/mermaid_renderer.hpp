#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace acecode::markdown {

// Semantic roles deliberately stay independent of FTXUI/theme state. The
// Markdown adapter maps them to the active terminal palette.
enum class MermaidRole {
    Border,
    NodeText,
    Edge,
    EdgeLabel,
    Title,
    Hint,
};

struct MermaidSpan {
    std::string text;
    MermaidRole role = MermaidRole::NodeText;
    bool italic = false;
};

struct MermaidLine {
    std::vector<MermaidSpan> spans;

    std::string plain_text() const;
    int display_width() const;
};

struct MermaidArt {
    std::vector<MermaidLine> lines;
    bool fallback = false;
    bool too_wide = false;
};

// Returns nullopt only for an empty/whitespace-only source. All other input is
// represented either as terminal diagram art or as a framed source fallback.
std::optional<MermaidArt> render_mermaid_terminal(
    std::string_view source,
    std::optional<int> max_width = std::nullopt);

} // namespace acecode::markdown
