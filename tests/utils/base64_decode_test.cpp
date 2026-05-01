// 覆盖 src/utils/base64.hpp 中新增的 base64_decode() 函数。
// encoder 已有覆盖(OSC 52 复制场景),这里只针对 decoder。
//
// 用例分两类:
//   - 标准合法输入(空 / 三种 padding 长度 / 二进制字节 / URL-safe 变体)
//   - 非法输入(出格字符 / padding 后还有数据 / 长度不齐)→ nullopt
//
// 同时保留 encoder 的回归测试(避免共享 hpp 改坏 OSC 52 链路)。

#include <gtest/gtest.h>

#include "utils/base64.hpp"

#include <string>

using namespace acecode;

// === 合法输入 ===

// 场景:空串 → 空串
TEST(Base64Decode, EmptyInput) {
    auto out = base64_decode("");
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, "");
}

// 场景:RFC 4648 标准三种 padding 长度都正确解码
TEST(Base64Decode, StandardPaddingVariants) {
    auto a = base64_decode("TQ==");      // 1 字节
    auto b = base64_decode("TWE=");      // 2 字节
    auto c = base64_decode("TWFu");      // 3 字节
    ASSERT_TRUE(a.has_value()); EXPECT_EQ(*a, "M");
    ASSERT_TRUE(b.has_value()); EXPECT_EQ(*b, "Ma");
    ASSERT_TRUE(c.has_value()); EXPECT_EQ(*c, "Man");
}

// 场景:任意二进制字节 round-trip(encoder 早已覆盖,这里反向回收)
TEST(Base64Decode, BinaryBytes) {
    std::string raw = std::string("\x00\xff\x10", 3);
    auto out = base64_decode("AP8Q");
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, raw);
}

// 场景:Bing 跳转链最常见的 URL → base64 → URL round-trip
TEST(Base64Decode, BingRedirectorPayload) {
    // "https://example.com/path" 的标准 base64
    auto out = base64_decode("aHR0cHM6Ly9leGFtcGxlLmNvbS9wYXRo");
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, "https://example.com/path");
}

// 场景:padding 缺失也接受(URL-safe 变体常省略 '=')
TEST(Base64Decode, MissingPaddingAccepted) {
    auto a = base64_decode("TQ");        // 应为 "TQ=="
    auto b = base64_decode("TWE");       // 应为 "TWE="
    ASSERT_TRUE(a.has_value()); EXPECT_EQ(*a, "M");
    ASSERT_TRUE(b.has_value()); EXPECT_EQ(*b, "Ma");
}

// 场景:URL-safe 变体('-' 替 '+', '_' 替 '/')也解码
TEST(Base64Decode, UrlSafeAlphabet) {
    // 标准:"//A=" → 0xFF 0xF0;URL-safe:"__A="
    auto std_out = base64_decode("//A=");
    auto url_out = base64_decode("__A=");
    ASSERT_TRUE(std_out.has_value());
    ASSERT_TRUE(url_out.has_value());
    EXPECT_EQ(*std_out, *url_out);
    EXPECT_EQ(*std_out, std::string("\xFF\xF0", 2));
}

// 场景:中间嵌入空白(换行 / 空格 / tab)被忽略 — RFC 2045 兼容
TEST(Base64Decode, WhitespaceIgnored) {
    auto out = base64_decode("TWFu\n  TWFu");
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(*out, "ManMan");
}

// === 非法输入 ===

// 场景:超出 base64 字母表的字符 → nullopt
TEST(Base64Decode, IllegalCharactersRejected) {
    EXPECT_FALSE(base64_decode("@@@@").has_value());
    EXPECT_FALSE(base64_decode("TQ$=").has_value());
    EXPECT_FALSE(base64_decode("\xff\xfe").has_value());
}

// 场景:padding '=' 后又出现数据字符 → nullopt
TEST(Base64Decode, DataAfterPaddingRejected) {
    EXPECT_FALSE(base64_decode("TQ==M").has_value());
    EXPECT_FALSE(base64_decode("=TQ==").has_value());
}

// 场景:总位数不齐(模 4 余 1)→ 不是合法 base64
TEST(Base64Decode, InvalidGroupLengthRejected) {
    EXPECT_FALSE(base64_decode("T").has_value());     // 6 bits 单独存在
    EXPECT_FALSE(base64_decode("TWFuT").has_value()); // 30 bits = 3.75 字节,非法
}

// === Encoder 回归(确保 hpp 改动没破坏 OSC 52 链路)===

// 场景:encoder 关键 round-trip 仍然正确
TEST(Base64EncoderRegression, KnownVectors) {
    EXPECT_EQ(base64_encode(""), "");
    EXPECT_EQ(base64_encode("M"), "TQ==");
    EXPECT_EQ(base64_encode("Ma"), "TWE=");
    EXPECT_EQ(base64_encode("Man"), "TWFu");
    EXPECT_EQ(base64_encode(std::string("\x00\xff\x10", 3)), "AP8Q");
}
