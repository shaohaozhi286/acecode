#pragma once

// Shell 绕写防护的纯判定逻辑。file_edit 安全失败后,AgentLoop 会阻止模型用 bash
// 直接写同一个文件(绕过编码/换行安全),这里提供两个纯函数判定:命令是否提及某路径、
// 命令是否"像是在写文件"。抽成 header-only 以便单测覆盖(原先藏在 agent_loop.cpp
// 匿名 namespace 中不可测)。

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <string>

namespace acecode {

namespace shell_guard_detail {

inline std::string ascii_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

inline std::string normalize_path_for_command_match(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    return ascii_lower_copy(std::move(value));
}

inline bool contains_any_of(const std::string& value,
                            std::initializer_list<const char*> needles) {
    for (const char* needle : needles) {
        if (value.find(needle) != std::string::npos) return true;
    }
    return false;
}

} // namespace shell_guard_detail

inline bool command_mentions_path(const std::string& command, const std::string& path) {
    using shell_guard_detail::normalize_path_for_command_match;
    std::string cmd = normalize_path_for_command_match(command);
    std::string normalized_path = normalize_path_for_command_match(path);
    if (normalized_path.empty()) return false;
    if (cmd.find(normalized_path) != std::string::npos) return true;

    size_t slash = normalized_path.find_last_of('/');
    std::string basename = slash == std::string::npos
        ? normalized_path
        : normalized_path.substr(slash + 1);
    return basename.size() >= 4 && cmd.find(basename) != std::string::npos;
}

// 只认显式写入特征。解释器名本身(powershell/python)不是写入证据:把整个解释器
// 当写入会连纯读取命令一起拦截,堵死模型在编辑失败后用 shell 求证文件状态的路径
// (回归:`powershell -Command "(Get-Content ...)"` 被 "Shell write blocked" 误拦)。
// 解释器内的写文件 API(WriteAllText / write_text / .write( 等)单独列为特征,
// 防绕写能力不依赖解释器名。
inline bool command_looks_like_file_write(const std::string& command) {
    using shell_guard_detail::ascii_lower_copy;
    using shell_guard_detail::contains_any_of;
    const std::string cmd = ascii_lower_copy(command);
    return contains_any_of(cmd, {
        ">",            // 重定向(含 >>)
        "set-content", "out-file", "add-content", "tee ",
        "sed -i", "copy ", "move ",
        // PowerShell / .NET / Python 的脚本式写文件 API
        "writealltext", "writealllines", "write_text", "writelines", ".write("
    });
}

} // namespace acecode
