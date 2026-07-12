#include "tui/thinking_heartbeat.hpp"

#include <cmath>

namespace acecode { namespace tui {

std::string format_token_count_short(long long n) {
    if (n < 0) n = 0;
    if (n < 1000) return std::to_string(n);
    if (n < 10000) {
        // 一位小数 k:以"百"为单位四舍五入(1234 → 12 个 0.1k → "1.2k")。
        // 9950..9999 会进位到 100 个 0.1k,此时按 10k 档输出整数,
        // 避免出现 "10.0k" 这种破坏档位规则的形态。
        long long tenths = std::llround(static_cast<double>(n) / 100.0);
        if (tenths >= 100) return std::to_string(tenths / 10) + "k";
        return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + "k";
    }
    if (n < 1000000) return std::to_string(n / 1000) + "k"; // 整数 k,截断
    // 一位小数 m:回合内 token 极少到这个量级,保持一位小数即可。
    long long tenths = std::llround(static_cast<double>(n) / 100000.0);
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + "m";
}

std::string format_thinking_heartbeat(long elapsed_secs, long long elapsed_ms,
                                      long long confirmed_tokens,
                                      std::size_t streamed_chars_since_usage) {
    std::string out = "[" + std::to_string(elapsed_secs) + "s";
    // 读数 = 已确认累计 + 当前请求流式估算(chars/4),整回合单调递增。
    long long readout = confirmed_tokens +
        static_cast<long long>(streamed_chars_since_usage / 4);
    if (elapsed_ms >= kShowTokensAfterMs && readout > 0) {
        // "·" = U+00B7,"↓" = U+2193(UTF-8 字面量,源文件为 UTF-8 编译)。
        out += " \xC2\xB7 \xE2\x86\x93 " + format_token_count_short(readout) + " tokens";
    }
    out += "]";
    return out;
}

}} // namespace acecode::tui
