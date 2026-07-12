// 推理指示行内联心跳段纯函数的单测。
// 变更来源:openspec/changes/inline-thinking-heartbeat —— 等待 LLM 时在
// 波浪动画短语右侧渲染 "[49s · ↓ 1.2k tokens]",替换被删除的底部
// "○ Thinking 4s 179 tok" chip。

#include "tui/thinking_heartbeat.hpp"

#include <gtest/gtest.h>

using acecode::tui::format_thinking_heartbeat;
using acecode::tui::format_token_count_short;
using acecode::tui::kShowTokensAfterMs;

// U+00B7(·)与 U+2193(↓)的 UTF-8 字面量,与实现保持同一形态,
// 避免测试源文件编码差异带来的假阴性。
static const std::string kSep = " \xC2\xB7 \xE2\x86\x93 ";

// ---- format_token_count_short:档位与边界 ----

// 触发场景:回合 token 数还没到 1000。
// 期望行为:原样输出,不缩写。
TEST(FormatTokenCountShort, BelowThousandVerbatim) {
    EXPECT_EQ(format_token_count_short(0), "0");
    EXPECT_EQ(format_token_count_short(340), "340");
    EXPECT_EQ(format_token_count_short(999), "999");
}

// 触发场景:1000..9999 区间。
// 期望行为:一位小数 k,四舍五入(1234 → 1.2k;1250 → 1.3k)。
// 阈值说明:该区间数字仍小,保留一位小数才能看出增长趋势(心跳感)。
TEST(FormatTokenCountShort, ThousandsOneDecimal) {
    EXPECT_EQ(format_token_count_short(1000), "1.0k");
    EXPECT_EQ(format_token_count_short(1234), "1.2k");
    EXPECT_EQ(format_token_count_short(1250), "1.3k");
    EXPECT_EQ(format_token_count_short(9949), "9.9k");
}

// 触发场景:9950..9999 —— 一位小数四舍五入会进位到 10.0。
// 期望行为:直接切到 10k 档输出 "10k",不允许出现 "10.0k" 破坏档位规则。
// 回归说明:naive 实现(sprintf %.1f)在这个边界会输出 10.0k。
TEST(FormatTokenCountShort, RoundUpCrossesIntoIntegerBucket) {
    EXPECT_EQ(format_token_count_short(9950), "10k");
    EXPECT_EQ(format_token_count_short(9999), "10k");
}

// 触发场景:10000..999999 区间。
// 期望行为:整数 k,截断(15678 → 15k,不是 16k)——数字已够大,
// 小数位只剩噪音;截断保证读数永不虚高。
TEST(FormatTokenCountShort, TenThousandsIntegerTruncated) {
    EXPECT_EQ(format_token_count_short(10000), "10k");
    EXPECT_EQ(format_token_count_short(15678), "15k");
    EXPECT_EQ(format_token_count_short(999999), "999k");
}

// 触发场景:≥ 1e6(单回合极端长跑)。
// 期望行为:一位小数 m。
TEST(FormatTokenCountShort, MillionsOneDecimal) {
    EXPECT_EQ(format_token_count_short(1000000), "1.0m");
    EXPECT_EQ(format_token_count_short(1234567), "1.2m");
}

// 触发场景:调用方传入负数(理论上不该发生,防御)。
// 期望行为:按 0 处理,不产出 "-1" 之类的怪读数。
TEST(FormatTokenCountShort, NegativeClampedToZero) {
    EXPECT_EQ(format_token_count_short(-5), "0");
}

// ---- format_thinking_heartbeat:门槛与组装 ----

// 触发场景:回合刚开始(< 3s 防闪烁门槛),即使已有流式输出。
// 期望行为:只显示计时段 "[2s]",token 段被门槛压住 —— 避免秒级短回合
// 里 token 数一闪而过造成视觉噪音(门槛沿用原底部 chip 的 3000ms)。
TEST(FormatThinkingHeartbeat, BeforeGateTimerOnly) {
    EXPECT_EQ(format_thinking_heartbeat(2, 2000, 100, 400), "[2s]");
}

// 触发场景:过了门槛且有确认 token。
// 期望行为:完整心跳段,中括号 + · 分隔 + ↓ 前缀。
TEST(FormatThinkingHeartbeat, AfterGateFullSegment) {
    EXPECT_EQ(format_thinking_heartbeat(12, 12000, 340, 0),
              "[12s" + kSep + "340 tokens]");
}

// 触发场景:过了门槛但读数为 0(纯工具轮的间隙、首个 delta 未到)。
// 期望行为:退回只显示计时段 —— "↓ 0 tokens" 是无信息噪音。
TEST(FormatThinkingHeartbeat, ZeroReadoutSuppressesTokenPart) {
    EXPECT_EQ(format_thinking_heartbeat(5, 5000, 0, 0), "[5s]");
    EXPECT_EQ(format_thinking_heartbeat(5, 5000, 0, 3), "[5s]"); // 3/4 截断为 0
}

// 触发场景:多请求回合 —— 请求 1 已确认 320,请求 2 正在流式(400 字节)。
// 期望行为:读数 = 320 + 400/4 = 420,继续增长。
// 回归说明:旧底部 chip 的取数策略是 authoritative > 0 就永远只显示
// 上一请求的终值,长回合里数字冻住不动 —— 恰恰在用户最容易怀疑卡死的
// 场景下心跳失效。累计语义修复该 bug。
TEST(FormatThinkingHeartbeat, MultiRequestTurnKeepsCounting) {
    EXPECT_EQ(format_thinking_heartbeat(49, 49000, 320, 400),
              "[49s" + kSep + "420 tokens]");
}

// 触发场景:估算值参与显示。
// 期望行为:数字不带 ~ 前缀(用户决策:心跳不是账单)。
TEST(FormatThinkingHeartbeat, EstimateHasNoTildePrefix) {
    std::string s = format_thinking_heartbeat(4, 4000, 0, 800);
    EXPECT_EQ(s, "[4s" + kSep + "200 tokens]");
    EXPECT_EQ(s.find('~'), std::string::npos);
}

// 触发场景:读数进入缩写档位。
// 期望行为:心跳段内的数字走同一套缩写规则。
TEST(FormatThinkingHeartbeat, AbbreviationAppliesInsideSegment) {
    EXPECT_EQ(format_thinking_heartbeat(49, 49000, 1200, 40),
              "[49s" + kSep + "1.2k tokens]");
}

// 触发场景:门槛常量被误改。
// 期望行为:与 spec 钉死的 3000ms 一致(防闪烁窗口是产品决策,不是实现细节)。
TEST(FormatThinkingHeartbeat, GateConstantPinned) {
    EXPECT_EQ(kShowTokensAfterMs, 3000);
}
