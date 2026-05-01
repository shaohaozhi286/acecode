// 覆盖 src/tool/web_search/html_utils.{hpp,cpp} 的三个纯函数:
//   - html_decode_entities: 命名实体 / 十进制 / 十六进制数字引用 / 未知保留
//   - collapse_whitespace : 多空白合并 + trim
//   - truncate_with_ellipsis: 按 codepoint 截断 + UTF-8 多字节边界
//
// 这些是 DDG / Bing CN backend 解析 HTML 时共用的底层工具,bug 直接影响
// 工具结果的可读性,所以覆盖要密。

#include <gtest/gtest.h>

#include "tool/web_search/html_utils.hpp"

using namespace acecode::web_search;

// === html_decode_entities ===

// 场景:基础命名实体被解码,纯文本不变
TEST(HtmlDecodeEntities, NamedEntitiesBasic) {
    EXPECT_EQ(html_decode_entities("Rust &amp; Go"), "Rust & Go");
    EXPECT_EQ(html_decode_entities("&lt;div&gt;"), "<div>");
    EXPECT_EQ(html_decode_entities("say &quot;hi&quot;"), "say \"hi\"");
    EXPECT_EQ(html_decode_entities("it&apos;s"), "it's");
    EXPECT_EQ(html_decode_entities("plain text"), "plain text");
    EXPECT_EQ(html_decode_entities(""), "");
}

// 场景:十进制数字字符引用解码
TEST(HtmlDecodeEntities, DecimalNumericReferences) {
    EXPECT_EQ(html_decode_entities("C&#43;&#43;"), "C++");
    EXPECT_EQ(html_decode_entities("&#65;&#66;&#67;"), "ABC");
    // 高位 codepoint 走多字节 UTF-8(中:U+4E2D)
    EXPECT_EQ(html_decode_entities("&#20013;"), "中");
}

// 场景:十六进制数字字符引用解码,大小写都接受
TEST(HtmlDecodeEntities, HexNumericReferences) {
    EXPECT_EQ(html_decode_entities("&#x4E2D;&#x6587;"), "中文");
    EXPECT_EQ(html_decode_entities("&#x4e2d;"), "中");      // 小写 hex
    EXPECT_EQ(html_decode_entities("&#X41;"), "A");          // 大写 X 前缀
}

// 场景:未识别 / 损坏的实体保留原样,不阻塞
TEST(HtmlDecodeEntities, UnknownOrMalformedKeptVerbatim) {
    EXPECT_EQ(html_decode_entities("&unknown;"), "&unknown;");
    EXPECT_EQ(html_decode_entities("a & b"), "a & b");           // 缺 ';' 直接保留
    EXPECT_EQ(html_decode_entities("&#xZZZ;"), "&#xZZZ;");       // 非法 hex
    EXPECT_EQ(html_decode_entities("&#;"), "&#;");               // 空数字
    EXPECT_EQ(html_decode_entities("trailing &"), "trailing &"); // 末尾孤 &
}

// 场景:nbsp 折叠成普通空格(后续 collapse_whitespace 会进一步合并)
TEST(HtmlDecodeEntities, NbspBecomesSpace) {
    EXPECT_EQ(html_decode_entities("a&nbsp;b"), "a b");
}

// === collapse_whitespace ===

// 场景:常规多空白 / tab / 换行折成单空格
TEST(CollapseWhitespace, MultipleSpacesCollapse) {
    EXPECT_EQ(collapse_whitespace("a   b"), "a b");
    EXPECT_EQ(collapse_whitespace("a\tb"), "a b");
    EXPECT_EQ(collapse_whitespace("a\nb"), "a b");
    EXPECT_EQ(collapse_whitespace("a \t \n b"), "a b");
    EXPECT_EQ(collapse_whitespace("Line one.\nLine two."), "Line one. Line two.");
}

// 场景:首尾空白被 trim
TEST(CollapseWhitespace, LeadingTrailingTrimmed) {
    EXPECT_EQ(collapse_whitespace("  hello  "), "hello");
    EXPECT_EQ(collapse_whitespace("\n\n\thi\n\n"), "hi");
    EXPECT_EQ(collapse_whitespace("only-no-spaces"), "only-no-spaces");
}

// 场景:边界 — 全空白 / 空串
TEST(CollapseWhitespace, EdgeCases) {
    EXPECT_EQ(collapse_whitespace(""), "");
    EXPECT_EQ(collapse_whitespace("     "), "");
    EXPECT_EQ(collapse_whitespace("\t\n\r"), "");
}

// === truncate_with_ellipsis ===

// 场景:短串原样返回(不加省略号)
TEST(TruncateWithEllipsis, ShortStringUnchanged) {
    EXPECT_EQ(truncate_with_ellipsis("hello", 10), "hello");
    EXPECT_EQ(truncate_with_ellipsis("", 10), "");
    EXPECT_EQ(truncate_with_ellipsis("exactly10!", 10), "exactly10!"); // 正好 10 不截
}

// 场景:超长 ASCII 串截断 + 加省略号(UTF-8 三字节)
TEST(TruncateWithEllipsis, LongAsciiTruncated) {
    auto out = truncate_with_ellipsis("abcdefghijklmnop", 5);
    // 5 个 codepoint + "…"(U+2026 编码 E2 80 A6)
    EXPECT_EQ(out, std::string("abcde\xE2\x80\xA6"));
}

// 场景:UTF-8 多字节字符按 codepoint 计数,不在序列中间切
TEST(TruncateWithEllipsis, Utf8MultibyteBoundary) {
    // "你好世界" 4 个 codepoint,每个 3 字节
    std::string s = "\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C";
    auto out = truncate_with_ellipsis(s, 2);
    // 截留前 2 个 codepoint:"你好" + "…"
    EXPECT_EQ(out, std::string("\xE4\xBD\xA0\xE5\xA5\xBD\xE2\x80\xA6"));
}

// 场景:max_codepoints == 0 返回空串
TEST(TruncateWithEllipsis, ZeroMax) {
    EXPECT_EQ(truncate_with_ellipsis("anything", 0), "");
}
