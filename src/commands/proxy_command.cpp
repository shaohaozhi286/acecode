// /proxy 命令实现。所有运行时操作都委托给 network::proxy_resolver();
// 文案构造抽到 format_proxy_display() 以便单测覆盖。

#include "commands/proxy_command.hpp"

#include "network/proxy_resolver.hpp"

#include <mutex>
#include <sstream>
#include <string>

namespace acecode {

namespace {

std::string trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    size_t end = s.size();
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

ProxyDisplaySnapshot snapshot_for_display(const AppConfig& cfg) {
    ProxyDisplaySnapshot snap;
    // 用一个代表性的 https url 让 resolver 决定该走哪一档代理 —— 启动横幅 +
    // /proxy 命令显示的就是"对 LLM API 的代理"语义,所以选 https。
    auto resolved = network::proxy_resolver().effective("https://example.com");
    snap.effective_url = resolved.url;
    snap.source = resolved.source;
    snap.mode = cfg.network.proxy_mode;
    snap.ca_bundle = cfg.network.proxy_ca_bundle;
    snap.insecure = cfg.network.proxy_insecure_skip_verify;
    return snap;
}

void cmd_proxy(CommandContext& ctx, const std::string& args) {
    std::string sub = trim(args);

    if (sub == "refresh") {
        network::proxy_resolver().refresh();
        emit(ctx, "Proxy detection refreshed.\n" +
                  format_proxy_display(snapshot_for_display(ctx.config)));
        return;
    }
    if (sub == "off") {
        network::proxy_resolver().set_session_override_off();
        emit(ctx, "Proxy temporarily disabled (session-only).\n" +
                  format_proxy_display(snapshot_for_display(ctx.config)));
        return;
    }
    if (sub == "reset") {
        network::proxy_resolver().reset_session_override();
        emit(ctx, "Proxy session override cleared.\n" +
                  format_proxy_display(snapshot_for_display(ctx.config)));
        return;
    }
    if (sub.rfind("set", 0) == 0) {
        // /proxy set <url>
        std::string rest = trim(sub.size() > 3 ? sub.substr(3) : "");
        if (rest.empty()) {
            emit(ctx, "Usage: /proxy set <url>");
            return;
        }
        // normalize 之前先脱敏:用户输入直接带凭据时,日志里不要原样保留。
        std::string normalized = network::normalize_proxy_url(rest);
        if (normalized.empty()) {
            emit(ctx, "Invalid proxy URL: " + network::redact_credentials(rest));
            return;
        }
        network::proxy_resolver().set_session_override_url(normalized);
        emit(ctx, "Proxy temporarily set to " +
                  network::redact_credentials(normalized) +
                  " (session-only).\n" +
                  format_proxy_display(snapshot_for_display(ctx.config)));
        return;
    }
    if (!sub.empty() && sub != "show") {
        emit(ctx, "Unknown subcommand. Usage: /proxy [refresh|off|set <url>|reset]");
        return;
    }

    // 默认:无参 / "show" → 显示当前状态
    emit(ctx, format_proxy_display(snapshot_for_display(ctx.config)));
}

} // anonymous namespace

std::string format_proxy_display(const ProxyDisplaySnapshot& snap) {
    std::ostringstream oss;
    if (snap.effective_url.empty()) {
        oss << "Effective proxy : direct";
    } else {
        oss << "Effective proxy : " << network::redact_credentials(snap.effective_url);
    }
    oss << "\nSource          : " << snap.source
        << "\nMode (config)   : " << snap.mode
        << "\nCA bundle       : " << (snap.ca_bundle.empty() ? "(none)" : snap.ca_bundle)
        << "\nSkip TLS verify : " << (snap.insecure ? "yes" : "no");
    return oss.str();
}

void register_proxy_command(CommandRegistry& registry) {
    registry.register_command({
        "proxy",
        "Show or temporarily switch the HTTP proxy used for LLM/API requests",
        cmd_proxy,
    });
}

} // namespace acecode
