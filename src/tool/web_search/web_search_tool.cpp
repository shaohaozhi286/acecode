#include "web_search_tool.hpp"

#include "backend.hpp"
#include "backend_router.hpp"
#include "html_utils.hpp"
#include "tool/tool_icons.hpp"
#include "utils/logger.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <sstream>
#include <string>

namespace acecode::web_search {

namespace {

constexpr const char* kToolName = "web_search";

std::string trim(const std::string& s) {
    std::size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    std::size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(start, end - start);
}

std::string truncate(const std::string& s, std::size_t max) {
    if (s.size() <= max) return s;
    return s.substr(0, max - 1) + "\xE2\x80\xA6"; // U+2026
}

std::string error_kind_str(SearchError::Kind k) {
    switch (k) {
        case SearchError::Kind::Network:     return "Network";
        case SearchError::Kind::Parse:       return "Parse";
        case SearchError::Kind::RateLimited: return "RateLimited";
        case SearchError::Kind::Disabled:    return "Disabled";
    }
    return "Unknown";
}

ToolSummary make_success_summary(const std::string& query,
                                  const SearchResponse& resp) {
    ToolSummary s;
    s.icon = "\xF0\x9F\x94\x8D"; // 🔍
    s.verb = "web_search";
    std::string q = query;
    if (q.size() > 40) q = q.substr(0, 37) + "...";
    s.object = "\"" + q + "\"";
    s.metrics.push_back({"results", std::to_string(resp.hits.size())});
    s.metrics.push_back({"backend", resp.backend_name});
    s.metrics.push_back({"time", format_duration_compact(resp.duration_ms)});
    return s;
}

ToolSummary make_error_summary(const std::string& query, const SearchError& err) {
    ToolSummary s;
    s.icon = "\xE2\x9C\x97"; // ✗
    s.verb = "web_search";
    std::string q = query;
    if (q.size() > 40) q = q.substr(0, 37) + "...";
    s.object = "\"" + q + "\"";
    s.metrics.push_back({"failed", error_kind_str(err.kind)});
    s.metrics.push_back({"err", truncate(err.message, 80)});
    if (!err.backend_name.empty()) {
        s.metrics.push_back({"backend", err.backend_name});
    }
    return s;
}

ToolResult execute_search(const std::string& arguments_json,
                          const ToolContext& ctx,
                          BackendRouter* router,
                          const WebSearchConfig* cfg) {
    std::string query;
    int requested_limit = 0;

    try {
        auto args = nlohmann::json::parse(arguments_json);
        if (args.contains("query") && args["query"].is_string()) {
            query = trim(args["query"].get<std::string>());
        }
        if (args.contains("limit") && args["limit"].is_number_integer()) {
            requested_limit = args["limit"].get<int>();
        }
    } catch (const std::exception& e) {
        return ToolResult{std::string("Web search failed: invalid JSON arguments: ") +
                              e.what(),
                          false};
    }

    if (query.empty()) {
        SearchError err{SearchError::Kind::Disabled,
                        "query is required (got empty string)", ""};
        ToolResult r{format_error_text(err, nullptr), false};
        r.summary = make_error_summary("", err);
        return r;
    }

    int hard_max = std::max(1, std::min(cfg->max_results, 10));
    int limit = requested_limit > 0 ? requested_limit : 5;
    if (limit < 1) limit = 1;
    if (limit > hard_max) limit = hard_max;

    NotifyFn notify = nullptr;
    if (ctx.stream) {
        notify = [&ctx](const std::string& m) { ctx.stream(m + "\n"); };
    }

    auto out = router->search_with_fallback(query, limit, ctx.abort_flag, notify);
    if (std::holds_alternative<SearchError>(out)) {
        const auto& err = std::get<SearchError>(out);
        ToolResult r{format_error_text(err, nullptr), false};
        r.summary = make_error_summary(query, err);
        return r;
    }
    const auto& resp = std::get<SearchResponse>(out);
    ToolResult r{format_results_markdown(query, resp), true};
    r.summary = make_success_summary(query, resp);
    return r;
}

} // namespace

std::string format_results_markdown(const std::string& query,
                                    const SearchResponse& resp) {
    std::ostringstream oss;
    oss << "Web search results for \"" << query << "\" (" << resp.backend_name
        << ", " << resp.hits.size() << " results):\n";
    int idx = 1;
    for (const auto& h : resp.hits) {
        std::string snippet = collapse_whitespace(h.snippet); // 双保险:折叠 newline
        oss << "\n" << idx << ". **" << h.title << "** \xE2\x80\x94 " << h.url;
        if (!snippet.empty()) {
            oss << "\n   " << snippet;
        }
        oss << "\n";
        ++idx;
    }
    return oss.str();
}

std::string format_error_text(const SearchError& err,
                              const SearchError* fallback_err) {
    std::ostringstream oss;
    oss << "Web search failed: " << error_kind_str(err.kind);
    if (!err.backend_name.empty()) {
        oss << " error from " << err.backend_name;
    }
    oss << ": " << err.message << ".";
    if (fallback_err) {
        oss << "\nTried fallback ";
        if (!fallback_err->backend_name.empty()) {
            oss << fallback_err->backend_name;
        }
        oss << ": " << fallback_err->message << ".";
    }
    return oss.str();
}

ToolImpl create_web_search_tool(BackendRouter& router,
                                 const WebSearchConfig& cfg) {
    ToolDef def;
    def.name = kToolName;
    def.description =
        "Search the web for up-to-date information. Returns a list of search "
        "results with title, URL, and short snippet. Use this when the user "
        "asks about recent events, current versions, library documentation, "
        "error messages, or anything else that benefits from external sources "
        "newer than your training data.";
    def.parameters = nlohmann::json({
        {"type", "object"},
        {"properties", {
            {"query", {
                {"type", "string"},
                {"description", "The search query"}
            }},
            {"limit", {
                {"type", "integer"},
                {"description", "Maximum number of results (1..10, default 5)"},
                {"minimum", 1},
                {"maximum", 10}
            }}
        }},
        {"required", nlohmann::json::array({"query"})}
    });
    auto router_ptr = &router;
    auto cfg_ptr = &cfg;
    auto exec = [router_ptr, cfg_ptr](const std::string& args_json,
                                       const ToolContext& ctx) -> ToolResult {
        return execute_search(args_json, ctx, router_ptr, cfg_ptr);
    };
    return ToolImpl{def, std::move(exec), /*is_read_only=*/true};
}

} // namespace acecode::web_search
