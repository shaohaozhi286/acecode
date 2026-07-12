#pragma once
// 推理指示行内联心跳段的纯文本组装(无 FTXUI 依赖,进 acecode_testable 单测)。
// 等待 LLM 时在波浪动画短语右侧渲染 "[49s · ↓ 1.2k tokens]" 风格的数据段,
// 中括号刻意与 Claude Code 的圆括号风格区分。设计见
// openspec/changes/inline-thinking-heartbeat/design.md。

#include <cstddef>
#include <string>

namespace acecode { namespace tui {

// token 段出现的耗时门槛(毫秒):短回合(< 3s)不闪 token 数,
// 长等待尽早给出心跳。原 tool_progress.cpp 的 SHOW_TOKENS_AFTER_MS 迁入。
inline constexpr long long kShowTokensAfterMs = 3000;

// token 数缩写:< 1000 原样;1000..9999 一位小数 k(1.2k,四舍五入,
// 进位到 10.0k 时切整数 "10k");10000..999999 整数 k(12k,截断);
// >= 1e6 一位小数 m(1.2m)。
std::string format_token_count_short(long long n);

// 组装心跳段完整文本(不含前导空格):
//   耗时 < kShowTokensAfterMs 或读数为 0 → "[Ns]"
//   否则                                  → "[Ns · ↓ X tokens]"
// 读数 = confirmed_tokens(回合累计已确认 completion_tokens)
//       + streamed_chars_since_usage / 4(自上次 usage 以来的流式估算)。
// 估算部分刻意不带 ~ 前缀:这个数字的作用是"没卡死"的心跳,不是账单。
std::string format_thinking_heartbeat(long elapsed_secs, long long elapsed_ms,
                                      long long confirmed_tokens,
                                      std::size_t streamed_chars_since_usage);

}} // namespace acecode::tui
