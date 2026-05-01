// confirm_question.cpp
// 见 confirm_question.hpp 的注释。本实现给三类常见工具(bash / file_write /
// file_edit)生成多行结构化的 confirm 标题:
//
//   bash:
//     Do you want to run this command?
//       $ <第 1 行命令(≤120 字符)>
//         <第 2 行命令>            (最多 3 行,溢出显示 "    ...")
//
//   file_write:
//     Do you want to write to <path>?
//       <N> line(s), <M> byte(s)
//
//   file_edit:
//     Do you want to edit <path>?
//       - <old_string 第一行,≤60 字符>
//       + <new_string 第一行,≤60 字符>
//
//   其他/异常:
//     Do you want to use <tool_name>?
//
// 路径截断阈值从早期的 40 放宽到 80,因为 confirm 弹窗比工具调用 preview
// 行宽,且用户决策时需要看到尽量完整的目标路径。命令截断阈值 120,允许多行
// 但只显示前 3 行,避免长 heredoc / pipeline 把 overlay 撑到屏幕外。

#include "tui/confirm_question.hpp"

#include <nlohmann/json.hpp>

namespace acecode::tui {

namespace {

// 路径太长时头部省略,保留尾部文件名(用户最关心的部分)。
std::string truncate_path(std::string p) {
    if (p.size() > 80) p = "..." + p.substr(p.size() - 77);
    return p;
}

// 单行命令尾部省略。
std::string truncate_command_line(std::string line) {
    if (line.size() > 120) line = line.substr(0, 117) + "...";
    return line;
}

// 多行命令格式化为带前缀的 block:首行 "  $ ",后续行 "    "(4 空格对齐)。
// 最多 3 行,如还有更多行,在末尾追加 "    ...";单行命令直接返回 "  $ <line>"。
std::string format_command_block(const std::string& cmd) {
    std::string out;
    size_t pos = 0;
    int emitted = 0;
    bool more = false;
    while (pos < cmd.size()) {
        size_t nl = cmd.find('\n', pos);
        std::string line = (nl == std::string::npos)
            ? cmd.substr(pos) : cmd.substr(pos, nl - pos);
        if (emitted >= 3) { more = true; break; }
        out += (emitted == 0 ? "  $ " : "    ");
        out += truncate_command_line(line);
        ++emitted;
        if (nl == std::string::npos) break;
        out += "\n";
        pos = nl + 1;
    }
    if (more) out += "\n    ...";
    return out;
}

// 取第一行并截断到 max 字符;若原字符串多行,在末尾加 "↵" 提示还有后续。
std::string truncate_first_line(const std::string& s, size_t max = 60) {
    size_t nl = s.find('\n');
    std::string line = (nl == std::string::npos) ? s : s.substr(0, nl);
    bool truncated_chars = false;
    if (line.size() > max) {
        line = line.substr(0, max - 3) + "...";
        truncated_chars = true;
    }
    bool has_more_lines = (nl != std::string::npos);
    if (has_more_lines && !truncated_chars) {
        line += " \xE2\x86\xB5";  // U+21B5 ↵
    }
    return line;
}

// 返回 content 的字节数与逻辑行数(末行不带 \n 也算一行)。
struct ContentStats { size_t bytes; int lines; };
ContentStats count_content_stats(const std::string& content) {
    ContentStats s;
    s.bytes = content.size();
    s.lines = 0;
    for (char c : content) if (c == '\n') ++s.lines;
    if (!content.empty() && content.back() != '\n') ++s.lines;
    return s;
}

std::string plural(int n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}
std::string plural(size_t n, const char* one, const char* many) {
    return std::to_string(n) + " " + (n == 1 ? one : many);
}

} // namespace

std::string build_confirm_question(const std::string& tool_name,
                                   const std::string& arguments_json) {
    try {
        auto j = nlohmann::json::parse(arguments_json);

        if (tool_name == "bash") {
            std::string out = "Do you want to run this command?";
            if (j.contains("command") && j["command"].is_string()) {
                std::string cmd = j["command"].get<std::string>();
                if (!cmd.empty()) out += "\n" + format_command_block(cmd);
            }
            return out;
        }

        if (tool_name == "file_write") {
            if (j.contains("file_path") && j["file_path"].is_string()) {
                std::string p = j["file_path"].get<std::string>();
                std::string out = "Do you want to write to " + truncate_path(p) + "?";
                if (j.contains("content") && j["content"].is_string()) {
                    auto s = count_content_stats(j["content"].get<std::string>());
                    out += "\n  " + plural(s.lines, "line", "lines")
                        +  ", " + plural(s.bytes, "byte", "bytes");
                }
                return out;
            }
        }

        if (tool_name == "file_edit") {
            if (j.contains("file_path") && j["file_path"].is_string()) {
                std::string p = j["file_path"].get<std::string>();
                std::string out = "Do you want to edit " + truncate_path(p) + "?";
                if (j.contains("old_string") && j["old_string"].is_string() &&
                    j.contains("new_string") && j["new_string"].is_string()) {
                    out += "\n  - " + truncate_first_line(j["old_string"].get<std::string>());
                    out += "\n  + " + truncate_first_line(j["new_string"].get<std::string>());
                }
                return out;
            }
        }
    } catch (...) {
        // 落到通用分支
    }

    return "Do you want to use " + tool_name + "?";
}

} // namespace acecode::tui
