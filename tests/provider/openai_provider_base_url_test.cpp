// 覆盖 src/provider/openai_provider.hpp 中 base_url 归一化(normalize_base_url)。
//
// 背景:用户在 /model 选模型 / 配 base_url 时经常多打或少打尾部斜杠。早期实现把
//   base_url 原样存下,拼端点时再固定追加 "/chat/completions"。当用户配成
//   "http://host/v1/"(带尾斜杠)时,最终 URL 变成 ".../v1//chat/completions",
//   不少自建网关(例如 example.internal)对双斜杠
//   直接回 404 {"detail":"Not Found"},表现为消息发出去立刻终止、无返回。
//   修复:存储 base_url 时统一裁掉所有尾部 '/' 与首尾空白,拼接端点时再补一个
//   前导 '/',使带不带尾斜杠都收敛到同一条合法 URL。
//
// 本文件覆盖的场景:
//   1. 带尾斜杠的 base_url 被裁掉,拼出的端点不含双斜杠(回归 404 bug)。
//   2. 多个连续尾斜杠("/v1///")全部裁掉。
//   3. 不带尾斜杠的 base_url 原样保留(不破坏正常配置)。
//   4. 复制粘贴常带进来的首尾空白被裁掉。
//   5. 空串 / 纯斜杠等边界输入不崩、结果稳定。

#include <gtest/gtest.h>

#include "provider/openai_provider.hpp"

using acecode::OpenAiCompatProvider;

// 场景 1:带尾斜杠 → 裁掉,端点不含双斜杠。
TEST(OpenAiBaseUrlNormalize, TrailingSlashStripped) {
    EXPECT_EQ(OpenAiCompatProvider::normalize_base_url("http://host/v1/"),
              "http://host/v1");
    // 模拟真实拼接,确认不再产生 "//chat/completions"。
    const std::string endpoint =
        OpenAiCompatProvider::normalize_base_url("http://host/v1/") + "/chat/completions";
    EXPECT_EQ(endpoint, "http://host/v1/chat/completions");
}

// 场景 2:多个连续尾斜杠全部裁掉。
TEST(OpenAiBaseUrlNormalize, MultipleTrailingSlashesStripped) {
    EXPECT_EQ(OpenAiCompatProvider::normalize_base_url("http://host/v1///"),
              "http://host/v1");
}

// 场景 3:无尾斜杠的正常配置原样保留。
TEST(OpenAiBaseUrlNormalize, NoTrailingSlashUnchanged) {
    EXPECT_EQ(OpenAiCompatProvider::normalize_base_url("http://host/v1"),
              "http://host/v1");
}

// 场景 4:首尾空白(复制粘贴常见)被裁掉,内部不动。
TEST(OpenAiBaseUrlNormalize, SurroundingWhitespaceTrimmed) {
    EXPECT_EQ(OpenAiCompatProvider::normalize_base_url("  http://host/v1/  "),
              "http://host/v1");
}

// 场景 5:边界输入不崩。空串保持空;纯斜杠裁成空串。
TEST(OpenAiBaseUrlNormalize, EdgeInputsStable) {
    EXPECT_EQ(OpenAiCompatProvider::normalize_base_url(""), "");
    EXPECT_EQ(OpenAiCompatProvider::normalize_base_url("///"), "");
    EXPECT_EQ(OpenAiCompatProvider::normalize_base_url("   "), "");
}
