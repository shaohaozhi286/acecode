// 覆盖 src/utils/base64.hpp 的 base64_encode。它是右键复制 OSC 52 走剪贴板的
// 唯一编码入口,任何编码 bug 都会把粘贴出去的内容损坏。本套测试锁定 RFC 4648
// 标准向量以及二进制字节(0x00 / 0xFF)不被当作"字符串结尾"误截断。

#include <gtest/gtest.h>

#include "utils/base64.hpp"

#include <string>

using acecode::base64_encode;

// 场景:空输入应得到空输出,不能返回 "=" 之类的错误 padding。
TEST(Base64Encode, EmptyString) {
    EXPECT_EQ(base64_encode(""), "");
}

// 场景:RFC 4648 的标准向量——三种 padding 数量(2/1/0 个 '=')各覆盖一次,
// 再补一条 "Hello, World!" 作为更长的常见样本。
TEST(Base64Encode, KnownVectors) {
    EXPECT_EQ(base64_encode("M"),   "TQ==");
    EXPECT_EQ(base64_encode("Ma"),  "TWE=");
    EXPECT_EQ(base64_encode("Man"), "TWFu");
    EXPECT_EQ(base64_encode("Hello, World!"), "SGVsbG8sIFdvcmxkIQ==");
}

// 场景:含 0x00 / 0xFF 的任意二进制输入必须按字节处理,绝不能因为 C 字符串
// 截断习惯在 NUL 处止步。
TEST(Base64Encode, BinarySafe) {
    std::string bin;
    bin.push_back('\x00');
    bin.push_back('\xff');
    bin.push_back('\x10');
    EXPECT_EQ(base64_encode(bin), "AP8Q");
}
