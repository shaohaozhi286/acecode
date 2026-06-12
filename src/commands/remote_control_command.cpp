#include "commands/remote_control_command.hpp"

#include "remote_control/remote_control_service.hpp"

#include <cctype>
#include <mutex>
#include <sstream>

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

RemoteControlDisplaySnapshot snapshot_for_display(const AppConfig& cfg) {
    auto status = rc::remote_control_service().status();
    RemoteControlDisplaySnapshot snap;
    snap.running = status.running;
    snap.port = status.running ? status.port : cfg.remote_control.port;
    snap.token = status.running ? status.token : cfg.remote_control.token;
    snap.outbound_url =
        status.running ? status.outbound_url : cfg.remote_control.outbound_url;
    snap.inbound_accepted = status.stats.inbound_accepted;
    snap.inbound_rejected = status.stats.inbound_rejected;
    snap.outbound_sent = status.stats.outbound_sent;
    snap.outbound_failed = status.stats.outbound_failed;
    snap.outbound_dropped = status.stats.outbound_dropped;
    return snap;
}

void cmd_remote_control(CommandContext& ctx, const std::string& args) {
    auto& service = rc::remote_control_service();
    std::string sub = trim(args);

    if (sub == "on") {
        if (service.running()) {
            emit(ctx, "Remote control is already on.\n" +
                      format_remote_control_display(snapshot_for_display(ctx.config)));
            return;
        }
        // token 持久化:首次启用生成一个并写回 config,IM 桥跨重启不用重新配对。
        if (ctx.config.remote_control.token.empty()) {
            ctx.config.remote_control.token = rc::generate_remote_control_token();
            save_config(ctx.config);
        }
        rc::RemoteControlOptions opts;
        opts.port = ctx.config.remote_control.port;
        opts.token = ctx.config.remote_control.token;
        opts.outbound_url = ctx.config.remote_control.outbound_url;
        if (ctx.session_manager) {
            opts.session_id = ctx.session_manager->current_session_id();
        }
        std::string error;
        if (!service.start(opts, &error)) {
            emit(ctx, "Failed to start remote control: " + error);
            return;
        }
        // 转发游标 = 当前对话长度:托管从现在开始,不向 IM 回放历史消息。
        {
            std::lock_guard<std::mutex> lk(ctx.state.mu);
            service.hub().set_forward_cursor(ctx.state.conversation.size());
        }
        emit(ctx, "Remote control started.\n" +
                  format_remote_control_display(snapshot_for_display(ctx.config)));
        return;
    }
    if (sub == "off") {
        if (!service.running()) {
            emit(ctx, "Remote control is not running.");
            return;
        }
        service.stop();
        emit(ctx, "Remote control stopped.");
        return;
    }
    if (sub.rfind("url", 0) == 0) {
        std::string rest = trim(sub.size() > 3 ? sub.substr(3) : "");
        if (rest.empty()) {
            emit(ctx, "Usage: /remote-control url <webhook-url>");
            return;
        }
        if (rest.rfind("http://", 0) != 0 && rest.rfind("https://", 0) != 0) {
            emit(ctx, "Outbound URL must start with http:// or https://");
            return;
        }
        ctx.config.remote_control.outbound_url = rest;
        save_config(ctx.config);
        service.set_outbound_url(rest);  // 运行中热更新,未运行只落配置
        emit(ctx, "Outbound webhook set to " + rest + "\n" +
                  format_remote_control_display(snapshot_for_display(ctx.config)));
        return;
    }
    if (!sub.empty() && sub != "show") {
        emit(ctx, "Unknown subcommand. Usage: /remote-control [on|off|url <webhook-url>]");
        return;
    }

    emit(ctx, format_remote_control_display(snapshot_for_display(ctx.config)));
}

} // anonymous namespace

std::string format_remote_control_display(const RemoteControlDisplaySnapshot& snap) {
    std::ostringstream oss;
    oss << "Remote control : " << (snap.running ? "ON" : "OFF");
    if (snap.running) {
        oss << "\nInbound        : POST http://127.0.0.1:" << snap.port << "/rc/send"
            << "\nToken header   : X-ACECode-RC-Token: " << snap.token
            << "\nBody           : {\"text\": \"<message>\"}"
            << "\nOutbound       : "
            << (snap.outbound_url.empty()
                    ? "(not configured — /remote-control url <webhook-url>)"
                    : snap.outbound_url)
            << "\nStats          : in " << snap.inbound_accepted << " ok / "
            << snap.inbound_rejected << " rejected | out " << snap.outbound_sent
            << " sent / " << snap.outbound_failed << " failed / "
            << snap.outbound_dropped << " dropped";
    } else {
        oss << "\nPort (config)  : " << snap.port
            << "\nOutbound       : "
            << (snap.outbound_url.empty() ? "(not configured)" : snap.outbound_url)
            << "\nRun /remote-control on to hand this session over to your IM bridge.";
    }
    return oss.str();
}

void register_remote_control_command(CommandRegistry& registry) {
    SlashCommand cmd{
        "remote-control",
        "Hand the current session over to an IM bridge (inbound HTTP + outbound webhook)",
        cmd_remote_control,
    };
    registry.register_command(cmd);
    // 短别名,与 Claude Code 的 /rc 习惯一致。
    cmd.name = "rc";
    cmd.description = "Alias for /remote-control";
    registry.register_command(cmd);
}

} // namespace acecode
