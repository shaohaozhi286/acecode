#pragma once

#include <string>
#include <vector>

namespace acecode::tui {

// Conservative visual width used by TUI truncation helpers. It follows the
// existing diff renderer rule: count UTF-8 codepoints, not East Asian Width.
int visual_width(const std::string& s);

// Shorten a string by replacing its middle with a single ellipsis. The
// retained width budget is biased toward the tail so basenames stay visible.
std::string truncate_middle(const std::string& s, int max_visual_width);

// Shorten a string by preserving its start and appending a single ellipsis.
// This is better for natural-language task text than middle truncation.
std::string truncate_end(const std::string& s, int max_visual_width);

// Generic width-bounded text truncation. Currently aliases tail truncation.
std::string truncate_to_width(const std::string& s, int max_visual_width);

// Wrap text into at most max_lines visual-width lines. If the input still
// overflows, the final line is tail-truncated with a single ellipsis.
std::vector<std::string> wrap_truncate_end(const std::string& s,
                                           int max_visual_width,
                                           std::size_t max_lines);

// Preserve prefix/suffix exactly and apply middle truncation only to the
// segment between them. If the fixed affixes already exceed the budget, the
// segment collapses to an ellipsis while affixes remain intact.
std::string truncate_middle_segment(const std::string& prefix,
                                    const std::string& segment,
                                    const std::string& suffix,
                                    int max_visual_width);

} // namespace acecode::tui
