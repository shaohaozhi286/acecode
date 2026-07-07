#pragma once

#include "tool_executor.hpp"

namespace acecode {

// LSP 查询工具(openspec add-lsp-service)。只在 config.lsp.enabled 时注册;
// 只读语义,免用户确认。九种操作经进程级 lsp::service() 聚合执行。
ToolImpl create_lsp_tool();

} // namespace acecode
