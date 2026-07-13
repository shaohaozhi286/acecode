#pragma once

// Shell 绕写防护的纯判定逻辑。file_edit 安全失败后,AgentLoop 会阻止模型用 bash
// 直接写同一个文件(绕过编码/换行安全),这里提供两个纯函数判定:命令是否提及某路径、
// 命令是否"像是在写文件"。抽成 header-only 以便单测覆盖(原先藏在 agent_loop.cpp
// 匿名 namespace 中不可测)。

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <string>
#include <vector>

#include "utils/path_validator.hpp"

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
        "sed -i", "copy ", "move ", "copy-item", "move-item", "cp ", "mv ",
        "remove-item", "new-item", "clear-content", "mkdir ", "rmdir ",
        "touch ", "rm ", "del ", "chmod ", "chown ",
        // PowerShell / .NET / Python 的脚本式写文件 API
        "writealltext", "writealllines", "write_text", "write_bytes",
        "writelines", ".write("
    });
}

inline std::vector<std::string> tokenize_shell_command(const std::string& command) {
    std::vector<std::string> out;
    std::string token;
    char quote = 0;
    auto flush = [&] {
        if (!token.empty()) out.push_back(std::move(token));
        token.clear();
    };
    for (std::size_t i = 0; i < command.size(); ++i) {
        const char c = command[i];
        if (quote) {
            if (c == quote) quote = 0;
            else token.push_back(c);
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c)) || c == ';' || c == '|') {
            flush();
            continue;
        }
        if (c == '>') {
            flush();
            std::string op(1, c);
            if (i + 1 < command.size() && command[i + 1] == '>') {
                op.push_back('>');
                ++i;
            }
            out.push_back(std::move(op));
            continue;
        }
        token.push_back(c);
    }
    flush();
    return out;
}

inline std::string trim_shell_target(std::string value) {
    while (!value.empty() && (value.front() == '(' || value.front() == '[' || value.front() == ',')) {
        value.erase(value.begin());
    }
    while (!value.empty() && (value.back() == ')' || value.back() == ']' ||
                              value.back() == ',' || value.back() == ';')) {
        value.pop_back();
    }
    return value;
}

inline bool shell_target_is_dynamic(const std::string& value) {
    return value.empty() || value.find('$') != std::string::npos ||
           value.find('%') != std::string::npos || value.find('*') != std::string::npos ||
           value.find('?') != std::string::npos;
}

inline bool shell_target_is_windows_absolute(const std::string& value) {
    return value.size() >= 3 &&
           std::isalpha(static_cast<unsigned char>(value[0])) &&
           value[1] == ':' && (value[2] == '/' || value[2] == '\\');
}

inline std::string normalize_windows_absolute_for_compare(std::string value) {
    value = shell_guard_detail::normalize_path_for_command_match(std::move(value));
    std::vector<std::string> segments;
    std::size_t pos = 0;
    while (pos <= value.size()) {
        std::size_t next = value.find('/', pos);
        if (next == std::string::npos) next = value.size();
        std::string segment = value.substr(pos, next - pos);
        if (segment == "..") {
            if (segments.size() > 1) segments.pop_back();
        } else if (!segment.empty() && segment != ".") {
            segments.push_back(std::move(segment));
        }
        pos = next + 1;
    }
    std::string normalized;
    for (const auto& segment : segments) {
        if (!normalized.empty()) normalized.push_back('/');
        normalized += segment;
    }
    return normalized;
}

inline bool windows_absolute_is_inside(const std::string& target,
                                       const std::string& working_dir) {
    if (!shell_target_is_windows_absolute(target) ||
        !shell_target_is_windows_absolute(working_dir)) {
        return false;
    }
    const std::string normalized_target =
        normalize_windows_absolute_for_compare(target);
    std::string normalized_root =
        normalize_windows_absolute_for_compare(working_dir);
    while (!normalized_root.empty() && normalized_root.back() == '/') {
        normalized_root.pop_back();
    }
    return normalized_target == normalized_root ||
           (normalized_target.size() > normalized_root.size() &&
            normalized_target.compare(0, normalized_root.size(), normalized_root) == 0 &&
            normalized_target[normalized_root.size()] == '/');
}

// LOOP Yolo is not a process sandbox, but explicit shell write targets must
// remain inside the active workspace/worktree root. This helper extracts the
// common redirection/cmdlet/copy destinations that command_looks_like_file_write
// recognizes. Empty result means the command is allowed; otherwise it is a
// fail-closed reason returned before process execution.
inline std::string loop_shell_write_escape_reason(const std::string& command,
                                                  const std::string& working_dir) {
    if (!command_looks_like_file_write(command)) return {};
    const auto tokens = tokenize_shell_command(command);
    std::vector<std::string> targets;
    bool requires_provable_target = false;

    auto lower = [](std::string value) {
        return shell_guard_detail::ascii_lower_copy(std::move(value));
    };
    // Shell/interpreter wrappers commonly put the actual script in one quoted
    // token. Inspect that statically visible inner command as well; otherwise
    // `powershell -Command "Copy-Item ... C:/outside"` would evade target
    // extraction merely because its whitespace was quoted.
    for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
        const std::string option = lower(tokens[i]);
        if (option == "-command" || option == "-c" || option == "/c") {
            const std::string& nested = tokens[i + 1];
            if (nested != command && command_looks_like_file_write(nested)) {
                const std::string nested_reason =
                    loop_shell_write_escape_reason(nested, working_dir);
                if (!nested_reason.empty()) return nested_reason;
            }
        }
    }
    auto take_after_option = [&](std::size_t command_index,
                                 std::initializer_list<const char*> options) {
        for (std::size_t i = command_index + 1; i + 1 < tokens.size(); ++i) {
            const std::string key = lower(tokens[i]);
            for (const char* option : options) {
                if (key == option) {
                    targets.push_back(tokens[i + 1]);
                    return true;
                }
            }
        }
        return false;
    };

    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const std::string key = lower(tokens[i]);
        if ((key == ">" || key == ">>") && i + 1 < tokens.size()) {
            targets.push_back(tokens[i + 1]);
            requires_provable_target = true;
            continue;
        }
        if (key == "set-content" || key == "add-content" || key == "out-file") {
            requires_provable_target = true;
            if (!take_after_option(i, {"-path", "-literalpath", "-filepath"}) &&
                i + 1 < tokens.size()) {
                targets.push_back(tokens[i + 1]);
            }
            continue;
        }
        if (key == "copy" || key == "move" || key == "copy-item" ||
            key == "move-item" || key == "cp" || key == "mv") {
            requires_provable_target = true;
            if (!take_after_option(i, {"-destination", "-dest"})) {
                for (std::size_t j = tokens.size(); j > i + 1; --j) {
                    if (!tokens[j - 1].empty() && tokens[j - 1][0] != '-') {
                        targets.push_back(tokens[j - 1]);
                        break;
                    }
                }
            }
            continue;
        }
        if (key == "remove-item" || key == "new-item" || key == "clear-content") {
            requires_provable_target = true;
            if (!take_after_option(i, {"-path", "-literalpath"}) && i + 1 < tokens.size()) {
                targets.push_back(tokens[i + 1]);
            }
            continue;
        }
        if (key == "mkdir" || key == "md" || key == "rmdir" || key == "touch" ||
            key == "rm" || key == "del" || key == "chmod" || key == "chown") {
            requires_provable_target = true;
            for (std::size_t j = tokens.size(); j > i + 1; --j) {
                if (!tokens[j - 1].empty() && tokens[j - 1][0] != '-') {
                    targets.push_back(tokens[j - 1]);
                    break;
                }
            }
            continue;
        }
        if (key == "sed" && i + 1 < tokens.size()) {
            const bool in_place = std::find(tokens.begin() + static_cast<std::ptrdiff_t>(i + 1),
                                            tokens.end(), "-i") != tokens.end();
            if (in_place) {
                requires_provable_target = true;
                targets.push_back(tokens.back());
            }
        }
    }

    const std::string command_lower = lower(command);
    if (targets.empty() && shell_guard_detail::contains_any_of(command_lower, {
            "writealltext", "writealllines", "write_text", "write_bytes",
            "writelines", ".write("
        })) {
        // Script-level APIs can compute their target dynamically. Without a
        // full language parser the only safe unattended policy is rejection.
        return "LOOP Yolo blocked a shell write whose target cannot be proven inside the work root";
    }
    if (requires_provable_target && targets.empty()) {
        return "LOOP Yolo blocked a shell write with no provable destination";
    }

    PathValidator validator(working_dir, false);
    for (auto target : targets) {
        target = trim_shell_target(std::move(target));
        if (shell_target_is_dynamic(target)) {
            return "LOOP Yolo blocked a dynamic shell write destination: " + target;
        }
        // std::filesystem follows the host OS. On Linux, a Windows absolute
        // path such as C:/outside is otherwise treated as relative and may be
        // incorrectly joined under the work root. Compare Windows paths
        // lexically so PowerShell/pwsh commands remain guarded on every host.
        if (shell_target_is_windows_absolute(target)) {
            if (!windows_absolute_is_inside(target, working_dir)) {
                return "LOOP Yolo blocked an external shell write: " + target;
            }
            continue;
        }
        const std::string rejection = validator.validate(target);
        if (!rejection.empty()) {
            return "LOOP Yolo blocked an external shell write: " + target;
        }
    }
    return {};
}

} // namespace acecode
