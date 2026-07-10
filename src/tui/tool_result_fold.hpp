#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace acecode::tui {

struct ToolResultPreview {
    std::vector<std::string> lines;
    bool folded = false;
};

// Build a bounded preview using terminal cell width rather than only hard
// newlines. This keeps a long one-line JSON/MCP result from filling the view
// after the renderer applies soft wrapping.
ToolResultPreview fold_tool_result_preview(const std::string& content,
                                           int max_visual_width,
                                           std::size_t max_visual_lines);

} // namespace acecode::tui
