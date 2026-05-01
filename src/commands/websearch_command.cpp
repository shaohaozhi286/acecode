// /websearch 命令实现:UI 副作用通过 emit() 推回 TuiState,核心逻辑(状态
// 文本、子命令分发)抽到纯函数以便单测。

#include "commands/websearch_command.hpp"

#include "tool/web_search/backend_router.hpp"
#include "tool/web_search/region_detector.hpp"
#include "tool/web_search/runtime.hpp"

#include <ctime>
#include <mutex>
#include <sstream>
#include <string>

namespace acecode {

namespace {

std::string trim(const std::string& s) {
    std::size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    std::size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(start, end - start);
}

void emit(CommandContext& ctx, const std::string& text) {
    {
        std::lock_guard<std::mutex> lk(ctx.state.mu);
        ctx.state.conversation.push_back({"system", text, false});
        ctx.state.chat_follow_tail = true;
    }
    if (ctx.post_event) ctx.post_event();
}

WebSearchDisplaySnapshot snapshot_from_runtime() {
    WebSearchDisplaySnapshot s;
    if (!web_search::is_initialized()) return s;
    auto& rt = web_search::runtime();
    s.active_backend = rt.router().active_name();
    s.config_backend = rt.cfg().backend;
    s.enabled = rt.cfg().enabled;
    auto region = rt.detector().cached_region();
    s.region = web_search::region_str(region);
    s.detected_at_ms = rt.detector().cached_detected_at_ms();
    s.registered = rt.router().registered_names_for_test();
    return s;
}

std::string format_iso8601(long long epoch_ms) {
    if (epoch_ms <= 0) return "(not detected)";
    std::time_t secs = static_cast<std::time_t>(epoch_ms / 1000);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &secs);
#else
    localtime_r(&secs, &tm_buf);
#endif
    char out[32];
    std::strftime(out, sizeof(out), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return std::string(out);
}

bool is_known_backend_name(const std::string& s) {
    return s == "duckduckgo" || s == "bing_cn" || s == "bochaai" || s == "tavily";
}

bool is_implemented_backend_name(const std::string& s) {
    return s == "duckduckgo" || s == "bing_cn";
}

void cmd_websearch(CommandContext& ctx, const std::string& args) {
    std::string sub = trim(args);
    auto out = dispatch_websearch_subcommand(sub);
    emit(ctx, out);
}

} // namespace

std::string format_websearch_status(const WebSearchDisplaySnapshot& snap) {
    std::ostringstream oss;
    oss << "Web Search:\n";
    oss << "  Enabled         : " << (snap.enabled ? "yes" : "no") << "\n";
    oss << "  Active backend  : "
        << (snap.active_backend.empty() ? "(none)" : snap.active_backend) << "\n";
    oss << "  Config backend  : " << snap.config_backend << "\n";
    oss << "  Region          : " << snap.region;
    if (snap.detected_at_ms > 0) {
        oss << " (detected " << format_iso8601(snap.detected_at_ms) << ")";
    }
    oss << "\n";
    oss << "  Registered      : ";
    for (std::size_t i = 0; i < snap.registered.size(); ++i) {
        if (i) oss << ", ";
        oss << snap.registered[i];
    }
    if (snap.registered.empty()) oss << "(none)";
    oss << "\n";
    return oss.str();
}

std::string dispatch_websearch_subcommand(const std::string& sub) {
    if (!web_search::is_initialized()) {
        return "Web search runtime is not initialized in this process.";
    }
    auto& rt = web_search::runtime();

    if (sub.empty() || sub == "show") {
        return format_websearch_status(snapshot_from_runtime());
    }
    if (sub == "refresh") {
        auto before = snapshot_from_runtime();
        rt.detector().invalidate();
        auto region = rt.detector().detect_now();
        rt.router().reset_to_config(region);
        std::ostringstream oss;
        oss << "Web search region re-detected: "
            << web_search::region_str(region) << "\n";
        oss << "Before: " << before.active_backend
            << " (region=" << before.region << ")\n";
        oss << "After : " << rt.router().active_name()
            << " (region=" << web_search::region_str(region) << ")";
        return oss.str();
    }
    if (sub.rfind("use", 0) == 0) {
        std::string rest = trim(sub.size() > 3 ? sub.substr(3) : "");
        if (rest.empty()) {
            return "Usage: /websearch use <backend>\n"
                   "Available: duckduckgo, bing_cn";
        }
        if (!is_known_backend_name(rest)) {
            return "Unknown backend '" + rest +
                   "'. Valid: duckduckgo, bing_cn (bochaai/tavily not implemented yet)";
        }
        if (!is_implemented_backend_name(rest)) {
            return "Backend '" + rest +
                   "' is not implemented yet. Use duckduckgo or bing_cn.";
        }
        if (!rt.router().set_active(rest)) {
            return "Backend '" + rest + "' is not registered in this process.";
        }
        return "Web search backend switched to " + rest + " (session-only).";
    }
    if (sub == "reset") {
        auto region = rt.detector().cached_region();
        rt.router().reset_to_config(region);
        return "Web search backend reset to config (active: " +
               rt.router().active_name() + ", region: " +
               web_search::region_str(region) + ").";
    }
    return "Unknown subcommand. Usage:\n"
           "  /websearch                Show current status\n"
           "  /websearch refresh        Re-detect region\n"
           "  /websearch use <backend>  Switch backend in this session\n"
           "  /websearch reset          Revert to config-derived backend";
}

void register_websearch_command(CommandRegistry& registry) {
    registry.register_command({
        "websearch",
        "Show or switch the web search backend (DuckDuckGo / Bing CN)",
        cmd_websearch,
    });
}

} // namespace acecode
