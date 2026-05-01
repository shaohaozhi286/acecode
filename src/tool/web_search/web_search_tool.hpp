#pragma once

// LLM-callable web_search 工具:封装 BackendRouter,把 LLM 入参解析、结果格式
// 化、ToolSummary 渲染等放在工具层,backend 实现保持纯净。
//
// 注册:main.cpp / daemon worker.cpp 在 cfg.web_search.enabled 时调
// register_web_search_tool(executor, router, cfg)。enabled=false 时不注册,
// LLM 看不到这个工具。

#include "backend.hpp"
#include "tool/tool_executor.hpp"
#include "config/config.hpp"

namespace acecode::web_search {

class BackendRouter;

// 创建工具实现。router 引用要在工具调用期间保持有效(进程生命周期)。
ToolImpl create_web_search_tool(BackendRouter& router, const WebSearchConfig& cfg);

// 暴露给单测的内部辅助:把 SearchResponse 渲染成给 LLM 的 markdown 文本。
std::string format_results_markdown(const std::string& query,
                                    const SearchResponse& resp);

// 渲染失败信息。primary_err 是直接错误;fallback_err 可选,描述对侧也失败。
std::string format_error_text(const SearchError& primary_err,
                              const SearchError* fallback_err);

} // namespace acecode::web_search
