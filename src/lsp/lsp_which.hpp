#pragma once

// PATH 上探测可执行文件。LSP server 只在探测命中时才会被启用,探测不到
// 静默跳过 —— 所以这里的语义必须和真实 spawn 一致:Windows 按 PATHEXT
// 尝试 .exe/.cmd/.bat 等扩展(npm 全局命令是 .cmd shim),POSIX 检查
// 可执行位。核心逻辑抽成可注入探针的纯函数,便于单测不依赖真实 PATH。

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace acecode::lsp {

// 文件存在性探针(true = 存在且是常规文件)。测试注入假文件系统。
using FileExistsFn = std::function<bool(const std::string& path)>;

// 纯逻辑核心:在给定 path_dirs 中查找 command。
// - command 含路径分隔符 → 只按给定路径(拼扩展)探测,不遍历 PATH。
// - pathext 为空(POSIX)→ 只按原名探测。
// - pathext 非空(Windows)→ 原名已带匹配扩展时直接探测,否则逐个拼接。
std::optional<std::string> which_in(const std::string& command,
                                    const std::vector<std::string>& path_dirs,
                                    const std::vector<std::string>& pathext,
                                    const FileExistsFn& file_exists);

// 生产入口:读取真实 PATH / PATHEXT / 文件系统。找不到返回 nullopt。
std::optional<std::string> which(const std::string& command);

} // namespace acecode::lsp
