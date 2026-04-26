#pragma once

#include <string>

namespace acecode::tui {

// Conservative visual width used by TUI truncation helpers. It follows the
// existing diff renderer rule: count UTF-8 codepoints, not East Asian Width.
int visual_width(const std::string& s);

// Shorten a string by replacing its middle with a single ellipsis. The
// retained width budget is biased toward the tail so basenames stay visible.
std::string truncate_middle(const std::string& s, int max_visual_width);

// Preserve prefix/suffix exactly and apply middle truncation only to the
// segment between them. If the fixed affixes already exceed the budget, the
// segment collapses to an ellipsis while affixes remain intact.
std::string truncate_middle_segment(const std::string& prefix,
                                    const std::string& segment,
                                    const std::string& suffix,
                                    int max_visual_width);

} // namespace acecode::tui
