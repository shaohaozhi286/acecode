#pragma once

// 诊断格式化(纯函数,单测覆盖)与 file_edit/file_write 的注入入口。
// 输出形态对齐 opencode packages/opencode/src/lsp/diagnostic.ts:
//   ERROR [line:col] message   (line/col 1-based)
//   <diagnostics file="...">...</diagnostics>
// 只透出 severity==1(ERROR)—— warning 噪声会诱导模型跑偏去修风格问题。

#include <atomic>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace acecode::lsp {

// 单文件透出上限;超出以 "... and N more" 收尾。
inline constexpr int kMaxDiagnosticsPerFile = 20;

// LSP Diagnostic JSON → 单行文本。severity 缺省按 ERROR 处理(LSP 规范
// 允许省略,惯例视为最高级)。
std::string pretty_diagnostic(const nlohmann::json& diagnostic);

// ERROR 级过滤 + 截断 + <diagnostics> 块组装。无 ERROR 返回空串。
std::string report_block(const std::string& display_path,
                         const std::vector<nlohmann::json>& diagnostics);

// file_edit / file_write 成功落盘后调用:LSP 未初始化 / 禁用 / 无匹配
// server 时立即返回(零延迟);否则 touch + 等 document 诊断(默认 5s,
// abort 以 ≤50ms 粒度生效),有 ERROR 则把提示块追加到 output 尾部。
void append_diagnostics_block(std::string& output,
                              const std::string& utf8_path,
                              const std::atomic<bool>* abort_flag);

} // namespace acecode::lsp
