#pragma once

// tool_call 出站消息的参数摘要 —— 纯函数,无 I/O、无线程依赖,便于单测。
//
// 规则:按 "command" → "file_path" → "url" 顺序在 arguments(JSON object)里
// 找字符串字段;都没有则取 arguments 里第一个字符串值;arguments 不是
// object,或 object 内没有任何字符串字段,则紧凑序列化整个 arguments。
// 结果按 UTF-8 字符边界截断到 kMaxArgsPreviewBytes 字节(不切多字节序列
// 中间),截断时追加省略号 "…"。

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>

namespace acecode::rc {

// 摘要预览的最大字节数(不含省略号本身)。
constexpr std::size_t kMaxArgsPreviewBytes = 80;

// tool_name 目前不参与摘要规则(所有工具走同一套字段优先级),保留在签名里
// 供未来按工具类型定制摘要逻辑。
std::string summarize_tool_args(const std::string& tool_name,
                                 const nlohmann::json& arguments);

} // namespace acecode::rc
