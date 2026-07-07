// /lsp 命令实现:状态文本组装是纯函数,TUI 与 daemon builtin 共用。

#include "commands/lsp_command.hpp"

#include <cctype>
#include <mutex>
#include <sstream>

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

void cmd_lsp(CommandContext& ctx, const std::string& args) {
    emit(ctx, dispatch_lsp_subcommand(trim(args)));
}

} // namespace

std::string format_lsp_status(const lsp::LspService::Status& status) {
    std::ostringstream oss;
    oss << "LSP:\n";
    oss << "  Enabled    : " << (status.enabled ? "yes" : "no") << "\n";
    if (!status.enabled) {
        oss << "  Enable with config.json: {\"lsp\": {\"enabled\": true}}\n";
        return oss.str();
    }

    oss << "  Connected  : ";
    if (status.connected.empty()) {
        oss << "(none — servers start lazily on first matching file)\n";
    } else {
        oss << "\n";
        for (const auto& entry : status.connected) {
            oss << "    * " << entry.server_id << "  root=" << entry.root
                << "  files=" << entry.open_files << "\n";
        }
    }
    if (!status.broken.empty()) {
        oss << "  Broken     : ";
        for (std::size_t i = 0; i < status.broken.size(); ++i) {
            if (i) oss << ", ";
            oss << status.broken[i].server_id << " (root=" << status.broken[i].root << ")";
        }
        oss << "  — failed to start; restart acecode to retry\n";
    }
    if (!status.not_installed.empty()) {
        oss << "  Not found  : ";
        for (std::size_t i = 0; i < status.not_installed.size(); ++i) {
            if (i) oss << ", ";
            oss << status.not_installed[i];
        }
        oss << "  — executable not on PATH; install to enable\n";
    }
    return oss.str();
}

std::string dispatch_lsp_subcommand(const std::string& sub) {
    if (!lsp::is_initialized()) {
        return "LSP runtime is not initialized in this process.";
    }
    if (sub.empty() || sub == "show" || sub == "status") {
        return format_lsp_status(lsp::service().status_snapshot());
    }
    return "Unknown subcommand. Usage:\n"
           "  /lsp    Show LSP integration status";
}

void register_lsp_command(CommandRegistry& registry) {
    registry.register_command({
        "lsp",
        "Show LSP server status (connected/broken/not installed)",
        cmd_lsp,
    });
}

} // namespace acecode
