// diff_line_wrap 单元测试。
//
// 背景 bug:TUI 的 diff 视图(file_write / file_edit 的 hunks)对超宽行
// 不换行 —— render_plain_line / render_word_diff_line 把整行塞进单个
// ftxui::text,超出终端宽度的部分被直接裁掉,用户看不到行尾内容
// (CJK 长行写入大段古文时整屏被截断)。修复方案是先按显示列宽把行
// 软换行成多条视觉行再渲染,本文件覆盖这层纯换行逻辑。

#include "tui/diff_line_wrap.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace acecode::tui {
namespace {

// 便捷构造:单段无强调文本。
std::vector<DiffWrapSpan> plain(const std::string& text) {
    return {{text, false}};
}

// 把一条视觉行的所有 span 拼回字符串,便于断言行内容。
std::string row_text(const DiffWrapRow& row) {
    std::string out;
    for (const auto& sp : row.spans) out += sp.text;
    return out;
}

// 触发场景:ASCII 行宽度不超过 max_width。
// 期望行为:原样返回一条视觉行,不产生多余空行。
TEST(DiffLineWrapTest, AsciiWithinWidthStaysSingleRow) {
    auto rows = wrap_diff_spans(plain("hello"), 10);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(row_text(rows[0]), "hello");
    EXPECT_EQ(rows[0].width, 5);
}

// 触发场景:行宽度恰好等于 max_width(边界)。
// 期望行为:仍是一条行,不能因 "==" 判成溢出而多出一条空行。
TEST(DiffLineWrapTest, ExactWidthDoesNotSpawnEmptyRow) {
    auto rows = wrap_diff_spans(plain("abcde"), 5);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(row_text(rows[0]), "abcde");
    EXPECT_EQ(rows[0].width, 5);
}

// 触发场景(回归,对应原始 bug):ASCII 行远超 max_width。
// 期望行为:按宽度切成多条行,内容一个字符不丢,每行宽度 ≤ max_width。
TEST(DiffLineWrapTest, LongAsciiLineWrapsWithoutLosingContent) {
    auto rows = wrap_diff_spans(plain("abcdefghij"), 4);
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(row_text(rows[0]), "abcd");
    EXPECT_EQ(row_text(rows[1]), "efgh");
    EXPECT_EQ(row_text(rows[2]), "ij");
    EXPECT_EQ(rows[0].width, 4);
    EXPECT_EQ(rows[1].width, 4);
    EXPECT_EQ(rows[2].width, 2);
}

// 触发场景:CJK 内容(每字 2 列)。修复前的 visual_width_bytes 把 CJK
// 按 1 列估算,即使做换行也会切在错误位置。
// 期望行为:按 East Asian Width 精算 —— 宽度 4 时每行恰好放 2 个汉字,
// 且绝不把一个多字节字符从中间切断。
TEST(DiffLineWrapTest, CjkWideGlyphsCountTwoColumns) {
    auto rows = wrap_diff_spans(plain("你好世界"), 4); // 你好世界
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(row_text(rows[0]), "你好"); // 你好
    EXPECT_EQ(row_text(rows[1]), "世界"); // 世界
    EXPECT_EQ(rows[0].width, 4);
    EXPECT_EQ(rows[1].width, 4);
}

// 触发场景:奇数列宽 + CJK,每行放下 1 个字后剩 1 列,放不下下一个
// 2 列宽字符。
// 期望行为:字符整体挪到下一行(每行只有 1 个字、实占 2 列),绝不为了
// 填满剩余 1 列把字符切半。
TEST(DiffLineWrapTest, WideGlyphNeverSplitsAcrossRows) {
    auto rows = wrap_diff_spans(plain("你好世"), 3); // 你好世
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(row_text(rows[0]), "你");
    EXPECT_EQ(row_text(rows[1]), "好");
    EXPECT_EQ(row_text(rows[2]), "世");
    for (const auto& row : rows) EXPECT_EQ(row.width, 2);

    // 补充断言:每行内容都是完整 UTF-8(无孤立续字节开头)。
    for (const auto& row : rows) {
        std::string t = row_text(row);
        ASSERT_FALSE(t.empty());
        EXPECT_NE(static_cast<unsigned char>(t[0]) & 0xC0, 0x80);
    }
}

// 触发场景:词级 diff 的多段输入(Same/强调交替),且在段中间换行。
// 期望行为:换行后每个 glyph 的强调归属不变;行内相邻同强调 glyph
// 合并回同一 span(渲染层每 span 一个 text 元素,避免碎片化)。
TEST(DiffLineWrapTest, EmphasisSurvivesWrapBoundary) {
    std::vector<DiffWrapSpan> spans = {
        {"aa", false},
        {"BBBB", true},
        {"cc", false},
    };
    auto rows = wrap_diff_spans(spans, 4);
    ASSERT_EQ(rows.size(), 2u);

    // 第 1 行:aa + BB(强调段被劈开,前半留在本行)
    ASSERT_EQ(rows[0].spans.size(), 2u);
    EXPECT_EQ(rows[0].spans[0].text, "aa");
    EXPECT_FALSE(rows[0].spans[0].emphasized);
    EXPECT_EQ(rows[0].spans[1].text, "BB");
    EXPECT_TRUE(rows[0].spans[1].emphasized);

    // 第 2 行:BB(强调延续)+ cc
    ASSERT_EQ(rows[1].spans.size(), 2u);
    EXPECT_EQ(rows[1].spans[0].text, "BB");
    EXPECT_TRUE(rows[1].spans[0].emphasized);
    EXPECT_EQ(rows[1].spans[1].text, "cc");
    EXPECT_FALSE(rows[1].spans[1].emphasized);
}

// 触发场景:diff 里出现空行(added/removed 的空行仍要渲染满宽色带)。
// 期望行为:返回恰好一条空行(width=0、无 span),而不是零条 ——
// 渲染层依赖这条空行画背景。
TEST(DiffLineWrapTest, EmptyInputYieldsSingleEmptyRow) {
    auto rows = wrap_diff_spans(plain(""), 10);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_TRUE(rows[0].spans.empty());
    EXPECT_EQ(rows[0].width, 0);

    // 全空段等价于空输入。
    rows = wrap_diff_spans({{"", false}, {"", true}}, 10);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_TRUE(rows[0].spans.empty());
}

// 触发场景:max_width=1 但内容含 2 列宽的 CJK 字符(极窄终端 /
// gutter 吃掉几乎全部宽度时 content_w 被钳到 1)。
// 期望行为:宽字符独占一行(行宽 2 > max_width,由调用方钳 padding),
// 不丢字符、不死循环。
TEST(DiffLineWrapTest, GlyphWiderThanMaxWidthGetsOwnRow) {
    auto rows = wrap_diff_spans(plain("你好"), 1); // 你好
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(row_text(rows[0]), "你");
    EXPECT_EQ(rows[0].width, 2);
    EXPECT_EQ(row_text(rows[1]), "好");
    EXPECT_EQ(rows[1].width, 2);
}

// 触发场景:非法 max_width(<1)。
// 期望行为:按 1 处理 —— 每个 ASCII 字符一行,不崩溃不死循环。
TEST(DiffLineWrapTest, NonPositiveWidthClampsToOne) {
    auto rows = wrap_diff_spans(plain("abc"), 0);
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(row_text(rows[0]), "a");
    EXPECT_EQ(row_text(rows[1]), "b");
    EXPECT_EQ(row_text(rows[2]), "c");
}

} // namespace
} // namespace acecode::tui
