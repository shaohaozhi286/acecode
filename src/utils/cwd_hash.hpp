#pragma once

// 把 cwd 路径映射成 16 位十六进制 hash,作为 .acecode/projects/<hash>/ 目录名。
//
// 抽出独立 util 是因为 desktop 的 workspace_registry 与 daemon 的 SessionStorage
// 必须用同一份算法 — 否则同一目录两边算出不同 hash,会出现"daemon 写了 sessions
// 到 abc123/,desktop 找不到该 workspace"的诡异错位。
//
// 算法不变(FNV-1a 64bit),前置规范化:
//   1) 若路径实际存在,先 weakly_canonical,把 C:/... 与 junction/subst 后的
//      N:/... 归到同一个真实目录 identity
//   2) 反斜杠→正斜杠 + 全部小写(Windows 大小写不敏感文件系统)+ 去尾斜杠。
// SessionStorage::compute_project_hash 委托到这里。

#include <string>

namespace acecode {

// 给定任意 cwd 字符串,返回 hash 前参与计算的规范化路径。线程安全。
std::string normalize_cwd_for_hash(const std::string& cwd);

// 给定任意 cwd 字符串,返回 16 字符小写十六进制 hash。线程安全。
std::string compute_cwd_hash(const std::string& cwd);

} // namespace acecode
