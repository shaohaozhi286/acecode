#pragma once

// -p / --print 无头模式的 CLI 参数解析(openspec add-headless-print-mode)。
// 纯函数,不碰任何全局状态,进 acecode_testable 单测。
//
// 语法(对齐 claude -p 的第 1 层能力,阶段 1 MVP):
//   acecode -p "prompt"                     位置参数作为 prompt
//   echo "prompt" | acecode -p              stdin 管道作为 prompt
//   git diff | acecode -p "review this"     两者同时给时拼接(stdin 在前)
//   acecode -p --yolo "fix the bug"         跳过所有权限确认
//   acecode -p --permission-mode accept-edits "..."
//   acecode -p --model my-model "..."       指定 saved_models 里的命名模型
//   acecode -p --max-turns 10 "..."         回合内迭代上限(0 = 跟随配置)

#include <string>
#include <vector>

namespace acecode::headless {

struct HeadlessCliOptions {
    bool print_mode = false;       // 出现过 -p / --print
    bool dangerous_mode = false;   // --yolo / --dangerous(与 TUI 同名参数对齐)
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

} // namespace acecode::headless
