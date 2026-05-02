#pragma once

// 把 cwd 路径映射成 16 位十六进制 hash,作为 .acecode/projects/<hash>/ 目录名。
//
// 抽出独立 util 是因为 desktop 的 workspace_registry 与 daemon 的 SessionStorage
// 必须用同一份算法 — 否则同一目录两边算出不同 hash,会出现"daemon 写了 sessions
// 到 abc123/,desktop 找不到该 workspace"的诡异错位。
//
// 算法不变(FNV-1a 64bit),前置规范化:反斜杠→正斜杠 + 全部小写(Windows 大小写不敏感
// 文件系统)+ 去尾斜杠。原实现来自 src/session/session_storage.cpp,这里只是抽函数,
// 行为完全一致;SessionStorage::compute_project_hash 现在委托过来。

#include <string>

namespace acecode {

// 给定任意 cwd 字符串,返回 16 字符小写十六进制 hash。线程安全,纯函数。
std::string compute_cwd_hash(const std::string& cwd);

} // namespace acecode
