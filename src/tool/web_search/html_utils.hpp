#pragma once

// 联网搜索 backend 共用的 HTML 文本处理工具。故意做得很轻 — 不引入 gumbo /
// libxml2 / lexbor 等 HTML parser 依赖,只覆盖 DDG / Bing CN 的固定 ASCII
// 标签结构所需的最小集。
//
// 设计取舍见 openspec/changes/add-web-search-tool/design.md Decision 2。

#include <cstddef>
#include <string>
#include <string_view>

namespace acecode::web_search {

// 解码 HTML 实体:`&amp;` `&lt;` `&gt;` `&quot;` `&apos;` `&nbsp;` 与十进制
// `&#NN;` / 十六进制 `&#xNN;` 数字字符引用。未识别的实体原样保留。
//
// 示例:
//   "Rust &amp; Go: &quot;async&quot;"        -> "Rust & Go: \"async\""
//   "C&#43;&#43;"                              -> "C++"
//   "&#x4E2D;&#x6587;"                         -> "中文"(UTF-8 编码)
std::string html_decode_entities(std::string_view in);

// 折叠任意空白(包括换行 / tab / 多重空格)为单个空格,并 trim 首尾。
// 用于把多行 snippet 收成一行,以保持工具结果文本不超长。
std::string collapse_whitespace(std::string_view in);

// 截断到指定 Unicode-codepoint 上限。超长时附加 "…"(U+2026,UTF-8 三字节)。
// 不在 UTF-8 多字节序列中间切。
std::string truncate_with_ellipsis(std::string_view in, std::size_t max_codepoints);

} // namespace acecode::web_search
