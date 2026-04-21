#pragma once

#include <optional>
#include <string>
#include <vector>

namespace acecode {

// Line-change counters derived while building a unified diff.
struct DiffStats {
    int additions = 0;
    int deletions = 0;
};

// 一条 diff 行的种类。和 claudecodehaha 里 `StructuredDiffFallback` 的
// 'add' / 'remove' / 'nochange' 一一对应。
enum class DiffLineKind {
    Context,
    Added,
    Removed,
};

// 结构化 diff 的单行:携带 marker、内容以及对应的 1-based 行号(纯增行
// `old_line_no` 为空,纯删行 `new_line_no` 为空)。
struct DiffLine {
    DiffLineKind kind = DiffLineKind::Context;
    std::string text; // 不含 '+' / '-' / ' ' 前缀
    std::optional<int> old_line_no;
    std::optional<int> new_line_no;
};

// 一个 hunk。行号起点 / 行数和 unified diff 的 `@@ -a,b +c,d @@` 对应。
struct DiffHunk {
    int old_start = 0; // 1-based;无 old 侧时为 0
    int old_count = 0;
    int new_start = 0;
    int new_count = 0;
    std::vector<DiffLine> lines;
};

// 生成结构化 diff。内容完全相同时返回空向量。
std::vector<DiffHunk> generate_structured_diff(
    const std::string& old_content,
    const std::string& new_content,
    const std::string& file_path
);

// Generate a unified diff between old and new content for a given file path.
std::string generate_unified_diff(
    const std::string& old_content,
    const std::string& new_content,
    const std::string& file_path
);

// Variant that also reports additions/deletions via an out-parameter so
// callers can populate ToolSummary.metrics without re-parsing the diff.
std::string generate_unified_diff(
    const std::string& old_content,
    const std::string& new_content,
    const std::string& file_path,
    DiffStats& stats
);

} // namespace acecode
