#include "fork_handler.hpp"

#include "../message_payload.hpp"

#include <algorithm>
#include <cctype>

namespace acecode::web {

namespace {

// 截断到至多 max_codepoints 个 Unicode codepoint,超出加 `…`。
// 简易 UTF-8 解码:lead byte 决定 sequence length(1/2/3/4)。
std::string truncate_utf8(const std::string& s, std::size_t max_codepoints) {
    std::size_t cp_count = 0;
    std::size_t i = 0;
    while (i < s.size()) {
        if (cp_count >= max_codepoints) break;
        unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t step;
        if      ((c & 0x80) == 0x00) step = 1;
        else if ((c & 0xE0) == 0xC0) step = 2;
        else if ((c & 0xF0) == 0xE0) step = 3;
        else if ((c & 0xF8) == 0xF0) step = 4;
        else                          step = 1;  // 不合法字节按 1 字节算
        if (i + step > s.size()) break;
        i += step;
        ++cp_count;
    }
    if (i >= s.size()) return s;
    return s.substr(0, i) + "\xE2\x80\xA6";  // ellipsis U+2026
}

bool is_blank(const std::string& s) {
    for (unsigned char c : s) {
        if (!std::isspace(c)) return false;
    }
    return true;
}

} // namespace

std::string compute_fork_title(const SessionMeta& source_meta,
                                const std::vector<SessionMeta>& sibling_metas,
                                const std::string& explicit_title) {
    // 显式 title:全空白视作未提供,走自动命名。
    if (!explicit_title.empty() && !is_blank(explicit_title)) {
        return explicit_title;
    }

    // sibling 计数:同 forked_from == source.id
    int sibling_count = 0;
    for (const auto& m : sibling_metas) {
        if (!m.forked_from.empty() && m.forked_from == source_meta.id) {
            ++sibling_count;
        }
    }
    int n = sibling_count + 1;

    // source title 选择:title > summary > 空(此时 source_text 留空,
    // 标题就是 `分叉N:`)
    std::string source_text = source_meta.title;
    if (source_text.empty()) source_text = source_meta.summary;

    constexpr std::size_t kMaxSourceCodepoints = 50;
    std::string truncated = truncate_utf8(source_text, kMaxSourceCodepoints);

    return "分叉" + std::to_string(n) + ":" + truncated;
}

std::optional<std::size_t> find_message_index_by_id(
    const std::vector<ChatMessage>& messages,
    const std::string& message_id) {
    if (message_id.empty()) return std::nullopt;
    for (std::size_t i = 0; i < messages.size(); ++i) {
        if (compute_message_id(messages[i]) == message_id) {
            return i;
        }
    }
    return std::nullopt;
}

} // namespace acecode::web
