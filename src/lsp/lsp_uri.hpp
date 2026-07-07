#pragma once

// 本地路径 <-> file:// URI 转换与诊断缓存 key 规范化。LSP 协议里文档
// 一律以 URI 标识;server 回推的 URI 与我们发出的可能大小写/分隔符/编码
// 不一致(Windows 驱动器盘符尤甚),缓存 key 必须走统一规范化。

#include <optional>
#include <string>

namespace acecode::lsp {

// UTF-8 绝对路径 → file URI。Windows: `C:\a b` → `file:///c:/a%20b`。
std::string path_to_file_uri(const std::string& utf8_path);

// file URI → UTF-8 本地路径(百分号解码;Windows 去掉前导斜杠、统一反斜杠)。
// 非 file scheme 返回 nullopt。
std::optional<std::string> file_uri_to_path(const std::string& uri);

// 诊断缓存/文件表的统一 key:分隔符统一为 '/';Windows 下全小写
// (NTFS 大小写不敏感,clangd 等会改写盘符大小写)。仅用于 map key,
// 不用于展示。
std::string normalize_path_key(const std::string& utf8_path);

} // namespace acecode::lsp
