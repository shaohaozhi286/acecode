#pragma once

// acecode -p / --print 无头运行器(openspec add-headless-print-mode)。
//
// 复刻 claude -p:跑完一个 agent 回合,stdout 打印最终 assistant 文本,退出码
// 表示成败。会话经 SessionRegistry 正常落盘(canonical JSONL + meta),事后可
// 用 `acecode --resume <id>` 或 Web UI 接管。
//
// 连续对话(每轮一个新进程,磁盘续接):`-c/--continue` 接当前 cwd 最近会话;
// `--resume <id>` 接指定会话(--model/--permission-mode 显式给出时覆盖会话保
// 存值);`--session-id <id>` 让脚本自定新会话 id 免解析 stdout;
// `--output-format json` 输出单个 result 对象(含 session_id)。
//
// bootstrap 是 daemon worker.cpp 的精简版:同一套 provider 解析 / 工具注册 /
// MCP / LSP / web_search / hooks / SessionRegistry,但不写 run/ 运行时文件、
// 不起心跳、不起 Crow HTTP/WS。
//
// 退出码:0 = 成功;1 = 回合失败(无 assistant 回复);64 = 用法错误(由
// main.cpp dispatch 层处理);130 = 用户 Ctrl+C 中止。

#include "headless_options.hpp"

namespace acecode::headless {

// 阻塞跑完整个 -p 生命周期。调用前提:opts.error 为空且 opts.print_mode=true。
int run_print_mode(const HeadlessCliOptions& opts);

} // namespace acecode::headless
