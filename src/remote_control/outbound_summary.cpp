#include "outbound_summary.hpp"

#include <array>

namespace acecode::rc {

namespace {

constexpr const char* kEllipsis = "\xE2\x80\xA6"; // "…"

bool is_utf8_continuation(unsigned char c) {
    return (c & 0xC0) == 0x80;
}

// 截断到 <= kMaxArgsPreviewBytes 字节;若截断点落在多字节序列中间,回退到
// 该序列起始位置之前,保证不产出半个 UTF-8 字符。
std::string truncate_preview(const std::string& s) {
    if (s.size() <= kMaxArgsPreviewBytes) return s;
    std::size_t cut = kMaxArgsPreviewBytes;
    while (cut > 0 && is_utf8_continuation(static_cast<unsigned char>(s[cut]))) {
        --cut;
    }
    return s.substr(0, cut) + kEllipsis;
}

// arguments 里按优先级顺序找到的第一个字符串字段;都不匹配返回 nullptr。
const std::string* find_priority_field(const nlohmann::json& arguments) {
    static constexpr std::array<const char*, 3> kPriorityKeys = {
        "command", "file_path", "url"};
    for (const char* key : kPriorityKeys) {
        auto it = arguments.find(key);
        if (it != arguments.end() && it->is_string()) {
            return &it->get_ref<const std::string&>();
        }
    }
    return nullptr;
}

// arguments 对象里第一个字符串值(遍历顺序取决于底层容器,当前为 nlohmann
// 默认 object 类型即按 key 字典序);找不到返回 nullptr。
const std::string* find_first_string_field(const nlohmann::json& arguments) {
    for (auto it = arguments.begin(); it != arguments.end(); ++it) {
        if (it->is_string()) {
            return &it->get_ref<const std::string&>();
        }
    }
    return nullptr;
}

} // namespace

std::string summarize_tool_args(const std::string& /*tool_name*/,
                                 const nlohmann::json& arguments) {
    if (arguments.is_object()) {
        if (const std::string* field = find_priority_field(arguments)) {
            return truncate_preview(*field);
        }
        if (const std::string* field = find_first_string_field(arguments)) {
            return truncate_preview(*field);
        }
    }
    return truncate_preview(arguments.dump());
}

} // namespace acecode::rc
