#pragma once

// -p / --print 无头模式的 CLI 参数解析(openspec add-headless-print-mode)。
// 纯函数,不碰任何全局状态,进 acecode_testable 单测。
//
// 语法(对齐 claude -p 的能力面):
//   acecode -p "prompt"                     位置参数作为 prompt
//   echo "prompt" | acecode -p              stdin 管道作为 prompt
//   git diff | acecode -p "review this"     两者同时给时拼接(stdin 在前)
//   acecode -p -c "continue"                接当前目录最近一个会话
//   acecode -p --resume <id> "next step"    接指定会话(id 必填 —— print 模式
//                                           有位置参数 prompt,裸 --resume 会把
//                                           prompt 吞成 id,所以"最近会话"语义
//                                           独立给 -c/--continue)
//   acecode -p --session-id my-task "..."   新会话用调用方自定的 id(脚本免
//                                           解析 stdout 即可确定性 --resume)
//   acecode -p --output-format json "..."   stdout 输出单个 result JSON 对象
//                                           (含 session_id,供脚本链式调用)
//   acecode -p --yolo "fix the bug"         跳过所有权限确认
//   acecode -p --permission-mode accept-edits "..."
//   acecode -p --model my-model "..."       指定 saved_models 里的命名模型
//   acecode -p --max-turns 10 "..."         回合内迭代上限(0 = 跟随配置)
//   acecode -p --help                       打印 print 模式帮助

#include <string>
#include <vector>

namespace acecode::headless {

struct HeadlessCliOptions {
    bool print_mode = false;       // 出现过 -p / --print
    bool show_help = false;        // -h / --help(print 模式子帮助,优先于执行)
    bool dangerous_mode = false;   // --yolo / --dangerous(与 TUI 同名参数对齐)
    bool continue_latest = false;  // -c / --continue:接当前 cwd 最近会话
    std::string resume_session_id; // --resume <id>:接指定会话
    std::string session_id;        // --session-id <id>:新会话自定 id
    std::string output_format;     // --output-format <text|json>,空 = text
    std::string permission_mode;   // --permission-mode <default|accept-edits|plan|yolo>
    std::string model_name;        // --model <saved_models.name>
    int max_turns = 0;             // --max-turns <n>,0 = 不覆盖配置
    std::string prompt;            // 位置参数(第一个非 flag token)
    std::string error;             // 非空 = 用法错误,调用方打印后 exit 64
};

// tokens = argv[1..](UTF-8)。只在 should_enter_print_mode 为 true 时调用。
// 未知 flag 一律报错而不是静默吞掉 —— -p 模式常被脚本调用,静默吞参数会让
// 拼写错误(--modle)变成难查的行为差异。
HeadlessCliOptions parse_headless_cli_options(const std::vector<std::string>& tokens);

// main.cpp dispatch 的入口判定:tokens 里出现 -p / --print,且第一个 token
// 不是已有子命令(configure 走 TUI 前置命令路径,daemon/service/upgrade 在
// dispatch 更早的分支已经 return,这里防御性排除一遍)。
bool should_enter_print_mode(const std::vector<std::string>& tokens);

// 会话 id token 的文件名安全校验(id 直接成为 <id>.jsonl 文件名):非空、
// ≤64 字符、仅 [A-Za-z0-9-_]。--resume / --session-id 的值都过这道门。
bool is_valid_session_id_token(const std::string& id);

// `acecode -p --help` 的完整帮助文本(多行,含用法/选项/连续对话示例/退出码)。
std::string print_mode_help();

// 用法一行版(参数错误时的 stderr 提示,末尾带 --help 指引)。
std::string print_mode_usage_line();

} // namespace acecode::headless
