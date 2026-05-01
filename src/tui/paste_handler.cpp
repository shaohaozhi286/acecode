// 多行粘贴折叠（fix-multiline-paste-input change）核心字符串实现。
// 头文件里讲了模块意图与函数语义，这里只实现。
#include "tui/paste_handler.hpp"

#include <algorithm>
#include <cstddef>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace acecode::tui {

PasteFeedResult PasteAccumulator::feed_special(const std::string& seq) {
    PasteFeedResult r;
    if (seq == kBracketedPasteBegin) {
        // 进入 paste 模式。如果上一次 paste 没正常结束（理论上不会，但防御一下），
        // 把 buffer 清掉。
        in_paste_ = true;
        buffer_.clear();
        r.consume = true;
        return r;
    }
    if (seq == kBracketedPasteEnd) {
        // 没在 paste 模式里却收到 end marker：吞掉以免被当成普通 special 下发，
        // 但不视为 just_completed。
        if (in_paste_) {
            in_paste_ = false;
            r.completed_text = normalize_pasted_text(buffer_);
            r.just_completed = true;
            buffer_.clear();
        }
        r.consume = true;
        return r;
    }
    if (in_paste_) {
        // 在 paste 模式内吃掉所有 special 事件。常见的：
        //   "\n" / "\r" — Return key inside paste（FTXUI 已 uniformize CR→LF）
        //   "\t"        — Tab inside paste
        //   "\x1b[..." — paste 内嵌的 CSI（罕见但合法 bytes，由 normalize 阶段剥离）
        // 全部 append 进 buffer，结束时由 normalize_pasted_text 一次清理。
        buffer_ += seq;
        r.consume = true;
        return r;
    }
    return r; // not in paste, not a marker — pass through
}

PasteFeedResult PasteAccumulator::feed_character(const std::string& chars) {
    PasteFeedResult r;
    if (in_paste_) {
        buffer_ += chars;
        r.consume = true;
    }
    return r;
}

void PasteAccumulator::reset() noexcept {
    in_paste_ = false;
    buffer_.clear();
}

namespace {

// 单次扫描：把 ESC 主导的转义序列剥掉，CRLF/CR → LF，tab → 4 空格，
// 其它 C0 控制字符（除 \n）丢弃。
//
// 不实现 C1 8-bit ESC 等价物（0x9B 等）—— 现代终端基本只发 7-bit ESC 形式，
// 而且 normalize 的输入是 FTXUI 已经处理过一遍的 buffer。
std::string strip_ansi_normalize_lines_tabs(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());

    constexpr std::size_t kTabExpandWidth = 4;

    for (std::size_t i = 0; i < raw.size();) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        if (c == 0x1B) { // ESC
            if (i + 1 >= raw.size()) {
                ++i; // 末尾的孤立 ESC，丢掉
                continue;
            }
            const unsigned char next = static_cast<unsigned char>(raw[i + 1]);
            if (next == '[') {
                // CSI: \x1b[ <params> <终止符 0x40..0x7E>
                std::size_t j = i + 2;
                while (j < raw.size()) {
                    const unsigned char cj = static_cast<unsigned char>(raw[j]);
                    if (cj >= 0x40 && cj <= 0x7E) { ++j; break; }
                    ++j;
                }
                i = j;
                continue;
            }
            if (next == ']') {
                // OSC: \x1b] ... 由 BEL (0x07) 或 ST (\x1b\\) 终止
                std::size_t j = i + 2;
                while (j < raw.size()) {
                    const unsigned char cj = static_cast<unsigned char>(raw[j]);
                    if (cj == 0x07) { ++j; break; }
                    if (cj == 0x1B && j + 1 < raw.size() && raw[j + 1] == '\\') {
                        j += 2; break;
                    }
                    ++j;
                }
                i = j;
                continue;
            }
            // ESC + X 双字节快捷（SS2/SS3/索引等）：跳过两字节
            i += 2;
            continue;
        }
        if (c == '\r') {
            // CRLF / CR → LF
            out.push_back('\n');
            ++i;
            if (i < raw.size() && raw[i] == '\n') ++i;
            continue;
        }
        if (c == '\t') {
            out.append(kTabExpandWidth, ' ');
            ++i;
            continue;
        }
        // 其它 C0 控制字符（0x00..0x1F 除了 \n）—— 保留 \n，丢弃其它
        if (c < 0x20 && c != '\n') {
            ++i;
            continue;
        }
        out.push_back(static_cast<char>(c));
        ++i;
    }
    return out;
}

} // namespace

std::string normalize_pasted_text(const std::string& raw) {
    return strip_ansi_normalize_lines_tabs(raw);
}

int count_newlines(const std::string& s) {
    int n = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '\n') {
            ++n;
        } else if (c == '\r') {
            ++n;
            if (i + 1 < s.size() && s[i + 1] == '\n') ++i;
        }
    }
    return n;
}

bool should_fold_to_placeholder(const std::string& normalized_text) {
    if (normalized_text.size() > kPlaceholderByteThreshold) return true;
    if (count_newlines(normalized_text) > kPlaceholderNewlineThreshold) return true;
    return false;
}

std::string format_placeholder(int paste_id, int newline_count) {
    std::ostringstream os;
    os << "[Pasted text #" << paste_id;
    if (newline_count > 0) {
        os << " +" << newline_count << " lines";
    }
    os << ']';
    return os.str();
}

namespace {
// 形如 [Pasted text #<digits>] 或 [Pasted text #<digits> +<digits> lines]。
// 注意 lines 的 +<n> 部分必须整体匹配（含前置空格），否则会把
// "[Pasted text #5 lines]" 这样畸形字符串混过去。
const std::regex& placeholder_regex() {
    static const std::regex re(R"(\[Pasted text #(\d+)(?: \+\d+ lines)?\])");
    return re;
}
} // namespace

std::vector<PlaceholderSpan> find_all_placeholders(const std::string& text) {
    std::vector<PlaceholderSpan> out;
    auto it = std::sregex_iterator(text.begin(), text.end(), placeholder_regex());
    const auto end = std::sregex_iterator();
    for (; it != end; ++it) {
        const std::smatch& m = *it;
        PlaceholderSpan span;
        span.begin = static_cast<std::size_t>(m.position(0));
        span.end = span.begin + static_cast<std::size_t>(m.length(0));
        try {
            span.paste_id = std::stoi(m[1].str());
        } catch (...) {
            span.paste_id = 0;
        }
        out.push_back(span);
    }
    return out;
}

std::vector<PlaceholderSpan> find_known_placeholders(
    const std::string& text,
    const std::map<int, std::string>& store) {
    std::vector<PlaceholderSpan> out;
    for (auto& span : find_all_placeholders(text)) {
        if (store.count(span.paste_id) > 0) {
            out.push_back(span);
        }
    }
    return out;
}

std::optional<PlaceholderSpan> placeholder_ending_at(
    const std::string& text,
    const std::map<int, std::string>& store,
    std::size_t byte_offset) {
    for (auto& span : find_known_placeholders(text, store)) {
        if (span.end == byte_offset) return span;
    }
    return std::nullopt;
}

std::optional<PlaceholderSpan> placeholder_starting_at(
    const std::string& text,
    const std::map<int, std::string>& store,
    std::size_t byte_offset) {
    for (auto& span : find_known_placeholders(text, store)) {
        if (span.begin == byte_offset) return span;
    }
    return std::nullopt;
}

std::string expand_placeholders(
    const std::string& text,
    const std::map<int, std::string>& store) {
    auto spans = find_all_placeholders(text);
    if (spans.empty()) return text;
    std::string out;
    out.reserve(text.size());
    std::size_t cursor = 0;
    for (auto& span : spans) {
        out.append(text, cursor, span.begin - cursor);
        auto it = store.find(span.paste_id);
        if (it != store.end()) {
            out.append(it->second);
        } else {
            // 未知 id：保留字面，让用户输入的 [Pasted text #99] 不被吞掉
            out.append(text, span.begin, span.end - span.begin);
        }
        cursor = span.end;
    }
    out.append(text, cursor, std::string::npos);
    return out;
}

void prune_unreferenced(
    std::map<int, std::string>& store,
    const std::string& text) {
    std::unordered_set<int> referenced;
    for (auto& span : find_all_placeholders(text)) {
        referenced.insert(span.paste_id);
    }
    for (auto it = store.begin(); it != store.end();) {
        if (referenced.count(it->first) == 0) {
            it = store.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace acecode::tui
