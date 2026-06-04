#pragma once

#include <string>
#include <vector>
#include <memory>

namespace acecode::markdown {

enum class TokenType {
    // Block-level
    Heading,
    Paragraph,
    Code,        // Fenced code block
    Blockquote,
    List,
    ListItem,
    Table,
    Hr,          // Thematic break
    Space,       // Empty line

    // Inline-level
    Text,
    Strong,      // **bold**
    Em,          // *italic*
    CodeSpan,    // `inline code`
    Link,        // [text](url)
    Image,       // ![alt](url)
    Br,          // Line break
    Escape,      // \char
    Html,        // Raw HTML (ignored)
    Del,         // ~strikethrough~ (disabled, ignored)
};

struct Token {
    TokenType type = TokenType::Text;
    std::string raw;          // Original source text
    std::string text;         // Processed text content
    int depth = 0;            // Heading level (1-6)
    std::string lang;         // Code block language
    std::string href;         // Link/Image URL
    bool ordered = false;     // List: ordered flag
    int start = 1;            // Ordered list: start number
    std::vector<Token> children;  // Sub-tokens (recursive)

    // Table-specific data
    std::vector<std::string> align;                          // Column alignments: "left", "center", "right", ""
    std::vector<std::vector<Token>> header_cells;            // header_cells[col] = inline tokens
    std::vector<std::vector<std::vector<Token>>> body_rows;  // body_rows[row][col] = inline tokens
};

struct FormatOptions {
    int terminal_width = 80;
    bool syntax_highlight = true;
    bool hyperlinks = true;
    bool strip_xml = true;
};

// Style attributes for a run of text (used in inline rendering)
struct TextStyle {
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool dim = false;
    bool is_code = false;       // Inline code span
    bool is_link = false;       // Link text (bright blue + underline)
    std::string href;           // For links
};

// A run of text with uniform styling
struct StyledRun {
    std::string text;
    TextStyle style;
};

} // namespace acecode::markdown
