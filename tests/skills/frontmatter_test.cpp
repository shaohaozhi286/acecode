// 覆盖 src/skills/frontmatter.cpp 的 YAML 块标量解析。
//
// 背景 bug:claude-code 生态的 skill(pdf / docx / kimi-webbridge 等)常用
// `description: >` 或 `description: |` 写多行描述。旧解析器把 ">" / "|"
// 当作值本身,后续缩进行按"意外缩进"丢弃 —— 设置页里这些技能的描述只显示
// 一个 ">" 或 "|" 字符。opencode 用真 YAML 解析器不受影响,ACECode 的手写
// 解析器需要显式支持块标量。

#include <gtest/gtest.h>

#include "skills/frontmatter.hpp"

namespace {

using acecode::get_string;
using acecode::parse_frontmatter;

// 场景: `description: >` folded 块 — 相邻行折叠成空格连接的一行。
// 回归表现:description == ">"。
TEST(FrontmatterBlockScalar, FoldedBlockJoinsLinesWithSpaces) {
    const std::string content =
        "---\n"
        "name: pdf\n"
        "description: >\n"
        "  Process PDF files.\n"
        "  Extract text and tables.\n"
        "---\n"
        "body\n";
    auto [fm, body] = parse_frontmatter(content);
    EXPECT_EQ(get_string(fm, "description"),
              "Process PDF files. Extract text and tables.");
    EXPECT_EQ(get_string(fm, "name"), "pdf");
    EXPECT_EQ(body, "body\n");
}

// 场景: `description: >-`(带 strip chomping)— 与 ">" 相同地折叠;
// chomping 只影响尾换行,元数据展示统一 strip。
TEST(FrontmatterBlockScalar, FoldedStripChompingAccepted) {
    const std::string content =
        "---\n"
        "description: >-\n"
        "  Line one\n"
        "  line two\n"
        "---\n";
    auto [fm, body] = parse_frontmatter(content);
    EXPECT_EQ(get_string(fm, "description"), "Line one line two");
}

// 场景: `description: |` literal 块 — 行以换行符保留。
// 回归表现:description == "|"。
TEST(FrontmatterBlockScalar, LiteralBlockPreservesNewlines) {
    const std::string content =
        "---\n"
        "name: kimi-webbridge\n"
        "description: |\n"
        "  First paragraph.\n"
        "  Second line.\n"
        "---\n";
    auto [fm, body] = parse_frontmatter(content);
    EXPECT_EQ(get_string(fm, "description"), "First paragraph.\nSecond line.");
}

// 场景: folded 块内的空行是段落分隔 → 折叠成一个换行,而不是被丢弃。
TEST(FrontmatterBlockScalar, FoldedBlankLineBecomesNewline) {
    const std::string content =
        "---\n"
        "description: >\n"
        "  Paragraph one\n"
        "  continues here.\n"
        "\n"
        "  Paragraph two.\n"
        "---\n";
    auto [fm, body] = parse_frontmatter(content);
    EXPECT_EQ(get_string(fm, "description"),
              "Paragraph one continues here.\nParagraph two.");
}

// 场景: 块标量后面还有兄弟 key — 块的边界判定不能吃掉后续 mapping 行。
TEST(FrontmatterBlockScalar, SiblingKeyAfterBlockStillParsed) {
    const std::string content =
        "---\n"
        "description: >-\n"
        "  Multi line\n"
        "  description text.\n"
        "whenToUse: run when asked\n"
        "---\n";
    auto [fm, body] = parse_frontmatter(content);
    EXPECT_EQ(get_string(fm, "description"), "Multi line description text.");
    EXPECT_EQ(get_string(fm, "whenToUse"), "run when asked");
}

// 场景: 普通值以 "|" 或 ">" 开头但不是块标量(如 "|foo")→ 按普通字符串。
TEST(FrontmatterBlockScalar, PipePrefixedPlainValueNotTreatedAsBlock) {
    const std::string content =
        "---\n"
        "description: |value-not-block\n"
        "---\n";
    auto [fm, body] = parse_frontmatter(content);
    EXPECT_EQ(get_string(fm, "description"), "|value-not-block");
}

// 场景: CRLF 行尾的 SKILL.md(Windows 编辑器常见)— getline 留下的行尾 \r
// 不能混进块标量内容。回归表现:pdf skill 的 description 中出现 "\r "。
TEST(FrontmatterBlockScalar, CrlfLineEndingsDoNotLeakCarriageReturns) {
    const std::string content =
        "---\r\n"
        "description: >\r\n"
        "  Line one.\r\n"
        "  Line two.\r\n"
        "---\r\n";
    auto [fm, body] = parse_frontmatter(content);
    EXPECT_EQ(get_string(fm, "description"), "Line one. Line two.");
}

// 场景: 空块(指示符后直接遇到同级 key)→ 空字符串,不吞兄弟 key。
TEST(FrontmatterBlockScalar, EmptyBlockYieldsEmptyString) {
    const std::string content =
        "---\n"
        "description: >\n"
        "name: still-here\n"
        "---\n";
    auto [fm, body] = parse_frontmatter(content);
    EXPECT_EQ(get_string(fm, "description"), "");
    EXPECT_EQ(get_string(fm, "name"), "still-here");
}

} // namespace
