// 多行粘贴折叠（fix-multiline-paste-input change）核心字符串逻辑。
// 这里没有 FTXUI 依赖，跑在 acecode_testable 里供 main.cpp 与单测共用：
//   - bracketed paste 协议常量 / 启停转义
//   - PasteAccumulator: bracketed paste 状态机（输入是已经被 FTXUI 解码过的
//     片段字符串：CSI 序列、字面字符或 Return/Tab 的 \n/\t）
//   - 完整粘贴文本归一化（去 ANSI、CRLF→LF、tab→4 spaces）
//   - 阈值决策（>800 字节 或 >2 个换行 → 折叠成 [Pasted text #N +M lines]）
//   - 占位符渲染 / 解析 / 展开
//   - 占位符 span 查找（供光标原子跨越与删除）
#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace acecode::tui {

// FTXUI 解码后送进来的 bracketed paste 标记（DECSET ?2004 协议）。
inline constexpr const char* kBracketedPasteBegin = "\x1b[200~";
inline constexpr const char* kBracketedPasteEnd   = "\x1b[201~";

// 主程序在进入 / 退出 TUI loop 时写到 stdout 的开关序列。
inline constexpr const char* kBracketedPasteEnableSeq  = "\x1b[?2004h";
inline constexpr const char* kBracketedPasteDisableSeq = "\x1b[?2004l";

// 阈值：>800 字节 或者 >2 个换行 → 折叠成 [Pasted text #N +M lines]。
inline constexpr std::size_t kPlaceholderByteThreshold = 800;
inline constexpr int kPlaceholderNewlineThreshold = 2;

// PasteAccumulator.feed_* 的返回结果。
//   consume        — true 表示 caller 应当吞掉这次事件（return true from CatchEvent）
//   just_completed — true 当且仅当此次 feed 是 paste 端结束。此时 completed_text
//                    已经填好 normalize 之后的完整文本（可能为空字符串）。
struct PasteFeedResult {
    bool consume = false;
    bool just_completed = false;
    std::string completed_text;
};

// bracketed paste 状态机。把 main.cpp 事件循环里 ESC[200~ … ESC[201~
// 之间的所有 FTXUI 事件聚合到内部 buffer，结束时一次性 normalize 出来。
class PasteAccumulator {
public:
    bool in_paste() const noexcept { return in_paste_; }

    // 喂入 FTXUI 的非字符事件（Event::Special / Event::Return / Event::Tab）。
    // `seq` 是 event.input()。begin marker / end marker / 任意 in-paste 事件都
    // 被吞掉；不在 paste 模式时返回 consume=false 让外层正常处理。
    PasteFeedResult feed_special(const std::string& seq);

    // 喂入 FTXUI 的 Character 事件 bytes（event.character()）。
    PasteFeedResult feed_character(const std::string& chars);

    // 测试与 shutdown 时清空。
    void reset() noexcept;

private:
    bool in_paste_ = false;
    std::string buffer_;
};

// 完整粘贴文本归一化：去 ANSI CSI / OSC 序列、CRLF / CR → LF、tab → 4 空格。
// 其余 0x00..0x1F 的 C0 控制字符（除 \n）也会被丢弃，避免把残留 ^A、^B 等
// 噪音字符塞进 input_text。
std::string normalize_pasted_text(const std::string& raw);

// 计算 \n / \r\n / \r 的"换行运行"个数。注意：是分隔符个数，不是行数。
// 例如 "a\nb\nc\nd" → 3。
int count_newlines(const std::string& s);

// 已归一化文本是否应折叠为占位符。
bool should_fold_to_placeholder(const std::string& normalized_text);

// 渲染占位符。newline_count==0 → "[Pasted text #N]"；否则 "[Pasted text #N +M lines]"。
std::string format_placeholder(int paste_id, int newline_count);

// 在 `text` 中定位的一个占位符 span。
struct PlaceholderSpan {
    std::size_t begin = 0;   // '[' 的字节偏移
    std::size_t end = 0;     // ']' 之后的字节偏移
    int paste_id = 0;
};

// 列出 text 中所有形如 [Pasted text #N] / [Pasted text #N +M lines] 的 span，
// 不区分 store 是否包含。expand_placeholders 用它判断哪些做替换、哪些保留字面。
std::vector<PlaceholderSpan> find_all_placeholders(const std::string& text);

// 仅返回 store 中存在 paste_id 的 span。给光标原子跨越 / 删除用。
std::vector<PlaceholderSpan> find_known_placeholders(
    const std::string& text,
    const std::map<int, std::string>& store);

// 如果光标 byte_offset 紧贴某个已知占位符的右端（end == byte_offset），返回该 span。
std::optional<PlaceholderSpan> placeholder_ending_at(
    const std::string& text,
    const std::map<int, std::string>& store,
    std::size_t byte_offset);

// 如果光标 byte_offset 紧贴某个已知占位符的左端（begin == byte_offset），返回该 span。
std::optional<PlaceholderSpan> placeholder_starting_at(
    const std::string& text,
    const std::map<int, std::string>& store,
    std::size_t byte_offset);

// 把 text 中已知占位符替换成 store 中的全文；未知 id 的占位符保持字面不动。
std::string expand_placeholders(
    const std::string& text,
    const std::map<int, std::string>& store);

// 删除 store 中所有不再被 text 引用的条目（input 被清空 / 被覆盖 / 被提交后调用）。
void prune_unreferenced(
    std::map<int, std::string>& store,
    const std::string& text);

} // namespace acecode::tui
