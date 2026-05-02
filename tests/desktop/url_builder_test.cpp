// 覆盖 src/desktop/url_builder.cpp 的纯字符串拼接逻辑。
// desktop shell 启动 daemon 子进程后,把这里输出的 URL 喂给 WebView,
// token 编错会让 WebView 携带错误 token 被 daemon 拒绝认证。

#include <gtest/gtest.h>

#include "desktop/url_builder.hpp"

using acecode::desktop::percent_encode;
using acecode::desktop::build_loopback_url;

// 场景: 全 unreserved 字符(A-Z a-z 0-9 - . _ ~) 不应被编码
TEST(DesktopUrlBuilder, PercentEncodeUnreservedPassthrough) {
    EXPECT_EQ(percent_encode("abcXYZ123-._~"), "abcXYZ123-._~");
}

// 场景: 空格 / + / / / = / % 等保留字符必须被编码,且十六进制大写两位
TEST(DesktopUrlBuilder, PercentEncodeReservedCharsEncoded) {
    EXPECT_EQ(percent_encode(" "), "%20");
    EXPECT_EQ(percent_encode("+"), "%2B");
    EXPECT_EQ(percent_encode("/"), "%2F");
    EXPECT_EQ(percent_encode("="), "%3D");
    EXPECT_EQ(percent_encode("%"), "%25");
}

// 场景: 高位字节(>127)按字节单独编码 — token 是 ASCII 但兜底字节安全
TEST(DesktopUrlBuilder, PercentEncodeHighByteEncoded) {
    std::string raw;
    raw.push_back(static_cast<char>(0xFF));
    EXPECT_EQ(percent_encode(raw), "%FF");
}

// 场景: 典型 daemon 生成的 token(假设含 base64-like 字符 + / =)整体应被编码后还原回原文
TEST(DesktopUrlBuilder, PercentEncodeBase64StyleToken) {
    std::string token = "abc+def/ghi=";
    EXPECT_EQ(percent_encode(token), "abc%2Bdef%2Fghi%3D");
}

// 场景: build_loopback_url 在合法 port + token 下生成预期 URL
TEST(DesktopUrlBuilder, BuildLoopbackUrlBasic) {
    EXPECT_EQ(build_loopback_url(28080, "secret"),
              "http://127.0.0.1:28080/?token=secret");
}

// 场景: build_loopback_url token 含特殊字符 → query 部分被 percent-encode
TEST(DesktopUrlBuilder, BuildLoopbackUrlEncodesToken) {
    EXPECT_EQ(build_loopback_url(49321, "a+b=c"),
              "http://127.0.0.1:49321/?token=a%2Bb%3Dc");
}

// 场景: token 为空 → 不带 ?token= 部分(degenerate path,仅供测试,生产不会走)
TEST(DesktopUrlBuilder, BuildLoopbackUrlOmitsEmptyToken) {
    EXPECT_EQ(build_loopback_url(8080, ""),
              "http://127.0.0.1:8080/");
}

// 场景: 高位 port(uint16 上限附近)拼接正常,不会被截
TEST(DesktopUrlBuilder, BuildLoopbackUrlHighPort) {
    EXPECT_EQ(build_loopback_url(65535, "t"),
              "http://127.0.0.1:65535/?token=t");
}
