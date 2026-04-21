#pragma once

#include <cstdlib>
#include <string>

namespace acecode {

// Returns the icon for a given tool verb. When the env var
// ACECODE_ASCII_ICONS is set to a non-empty non-"0" value, we return a plain
// ASCII glyph to sidestep terminals that don't render emoji cleanly.
//
// Known tool ids: "bash", "file_read", "file_write", "file_edit".
inline std::string tool_icon(const std::string& tool_id) {
    const char* ascii = std::getenv("ACECODE_ASCII_ICONS");
    const bool ascii_mode = ascii && ascii[0] != '\0' && !(ascii[0] == '0' && ascii[1] == '\0');
    if (ascii_mode) {
        if (tool_id == "bash") return "$";
        if (tool_id == "file_read") return "R";
        if (tool_id == "file_write") return "W";
        if (tool_id == "file_edit") return "E";
        return "*";
    }
    // Default: Unicode glyphs. Kept simple (single-codepoint arrows / glyph)
    // rather than full emoji, so they render at single width in most terminals.
    if (tool_id == "bash") return "$";              // shell prompt glyph
    if (tool_id == "file_read") return "\xE2\x86\x92";  // "→" (read)
    if (tool_id == "file_write") return "\xE2\x9C\x8D"; // "✍" (write)
    if (tool_id == "file_edit") return "\xE2\x9C\x8E";  // "✎" (edit)
    return "*";
}

// Format a byte count as a compact human-readable string: "42B" / "1.3KB" /
// "4.2MB". Used in summary metrics.
inline std::string format_bytes_compact(size_t n) {
    if (n < 1024) return std::to_string(n) + "B";
    double kb = static_cast<double>(n) / 1024.0;
    if (kb < 1024.0) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1fKB", kb);
        return buf;
    }
    double mb = kb / 1024.0;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1fMB", mb);
    return buf;
}

// Format a millisecond duration as "Xms" / "Y.Zs" / "Ns" for summary metrics.
inline std::string format_duration_compact(long long ms) {
    if (ms < 1000) return std::to_string(ms) + "ms";
    if (ms < 10000) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1fs", static_cast<double>(ms) / 1000.0);
        return buf;
    }
    long long sec = ms / 1000;
    return std::to_string(sec) + "s";
}

} // namespace acecode
