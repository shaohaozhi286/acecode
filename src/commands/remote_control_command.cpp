#include "commands/remote_control_command.hpp"

#include "remote_control/channel_plugin.hpp"
#include "remote_control/remote_control_service.hpp"

#include <cctype>
#include <mutex>
#include <optional>
#include <sstream>

#include <nlohmann/json.hpp>

namespace acecode {

namespace {

struct ActiveChannelBinding {
    std::string name;
    rc::ChannelPluginManifest manifest;
    std::string session_id;
    int timeout_ms = 10000;
};

std::mutex g_active_channel_mu;
std::optional<ActiveChannelBinding> g_active_channel;

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

std::string active_channel_name() {
    std::lock_guard<std::mutex> lk(g_active_channel_mu);
    return g_active_channel ? g_active_channel->name : std::string();
}

void set_active_channel(ActiveChannelBinding binding) {
    std::lock_guard<std::mutex> lk(g_active_channel_mu);
    g_active_channel = std::move(binding);
}

std::optional<ActiveChannelBinding> clear_active_channel() {
    std::lock_guard<std::mutex> lk(g_active_channel_mu);
    auto binding = std::move(g_active_channel);
    g_active_channel.reset();
    return binding;
}

std::string current_session_id(CommandContext& ctx) {
    return ctx.session_manager ? ctx.session_manager->current_session_id() : std::string();
}

RemoteControlDisplaySnapshot snapshot_for_display(const AppConfig& cfg) {
    auto status = rc::remote_control_service().status();
    RemoteControlDisplaySnapshot snap;
    snap.running = status.running;
    snap.port = status.running ? status.port : cfg.remote_control.port;
    snap.token = status.running ? status.token : cfg.remote_control.token;
    snap.outbound_url =
        status.running ? status.outbound_url : cfg.remote_control.outbound_url;
    snap.default_channel = cfg.remote_control.default_channel;
    snap.active_channel = active_channel_name();
    snap.inbound_accepted = status.stats.inbound_accepted;
    snap.inbound_rejected = status.stats.inbound_rejected;
    snap.outbound_sent = status.stats.outbound_sent;
    snap.outbound_failed = status.stats.outbound_failed;
    snap.outbound_dropped = status.stats.outbound_dropped;
    return snap;
}

void set_forward_cursor_to_current_conversation(CommandContext& ctx,
                                                rc::RemoteControlService& service) {
    std::lock_guard<std::mutex> lk(ctx.state.mu);
    service.hub().set_forward_cursor(ctx.state.conversation.size());
}

bool ensure_remote_control_running(CommandContext& ctx,
                                   rc::RemoteControlService& service,
                                   std::string* token,
                                   int* port,
                                   bool* started_now,
                                   std::string* error) {
    if (error) error->clear();
    if (started_now) *started_now = false;
    if (service.running()) {
        auto status = service.status();
        if (token) *token = status.token;
        if (port) *port = status.port;
        if (status.token.empty()) {
            if (error) *error = "remote control token is empty";
            return false;
        }
        return true;
    }

    std::string next_token = ctx.config.remote_control.token.empty()
        ? rc::generate_remote_control_token()
        : ctx.config.remote_control.token;
    if (next_token.empty()) {
        if (error) *error = "remote control token is empty";
        return false;
    }

    rc::RemoteControlOptions opts;
    opts.port = ctx.config.remote_control.port;
    opts.token = next_token;
    opts.outbound_url = ctx.config.remote_control.outbound_url;
    opts.session_id = current_session_id(ctx);
    if (!service.start(opts, error)) {
        return false;
    }
    set_forward_cursor_to_current_conversation(ctx, service);

    if (token) *token = next_token;
    if (port) *port = service.status().port;
    if (started_now) *started_now = true;
    return true;
}

void activate_default_channel(CommandContext& ctx) {
    const std::string channel_name = ctx.config.remote_control.default_channel;
    if (channel_name.empty()) {
        emit(ctx, format_remote_control_display(snapshot_for_display(ctx.config)));
        return;
    }

    auto channel_it = ctx.config.remote_control.channels.find(channel_name);
    if (channel_it == ctx.config.remote_control.channels.end()) {
        emit(ctx, "Default channel '" + channel_name +
                      "' is not configured under remote_control.channels.\n" +
                      format_remote_control_display(snapshot_for_display(ctx.config)));
        return;
    }

    const auto& channel_cfg = channel_it->second;
    std::string error;
    auto manifest = rc::load_channel_plugin_manifest(channel_cfg.manifest_path, &error);
    if (!manifest.has_value()) {
        emit(ctx, "Failed to load channel plugin '" + channel_name + "': " + error);
        return;
    }

    auto& service = rc::remote_control_service();
    std::string token;
    int port = 0;
    bool started_now = false;
    if (!ensure_remote_control_running(ctx, service, &token, &port, &started_now, &error)) {
        emit(ctx, "Failed to start remote control for channel '" + channel_name +
                      "': " + error);
        return;
    }

    rc::ChannelActivationRequest request;
    request.session_id = current_session_id(ctx);
    request.inbound_url = "http://127.0.0.1:" + std::to_string(port) + "/rc/send";
    request.token = token;
    request.settings = channel_cfg.settings.is_object()
        ? channel_cfg.settings
        : nlohmann::json::object();

    rc::ChannelPluginHost host;
    const int timeout_ms = channel_cfg.timeout_ms > 0
        ? channel_cfg.timeout_ms
        : manifest->timeout_ms;
    auto activation = host.activate(*manifest, request, timeout_ms, &error);
    if (!activation.ok) {
        if (started_now) service.stop();
        emit(ctx, "Failed to activate channel '" + channel_name + "': " + error);
        return;
    }

    if (ctx.config.remote_control.token != token) {
        ctx.config.remote_control.token = token;
        save_config(ctx.config);
    }
    service.set_outbound_url(activation.status.outbound_url);
    set_active_channel(ActiveChannelBinding{
        channel_name,
        *manifest,
        request.session_id,
        timeout_ms,
    });

    std::ostringstream out;
    out << "Channel '" << channel_name << "' connected.";
    if (activation.status.already_running) out << " Existing runtime reused.";
    out << "\n" << format_remote_control_display(snapshot_for_display(ctx.config));
    emit(ctx, out.str());
}

void deactivate_active_channel_best_effort(std::string* warning) {
    if (warning) warning->clear();
    auto active = clear_active_channel();
    if (!active.has_value()) return;
    std::string error;
    rc::ChannelPluginHost host;
    if (!host.deactivate(active->manifest, active->session_id, active->timeout_ms, &error) &&
        warning) {
        *warning = error;
    }
}

void cmd_remote_control(CommandContext& ctx, const std::string& args) {
    auto& service = rc::remote_control_service();
    std::string sub = trim(args);

    if (sub.empty()) {
        activate_default_channel(ctx);
        return;
    }

    if (sub == "on") {
        if (service.running()) {
            emit(ctx, "Remote control is already on.\n" +
                      format_remote_control_display(snapshot_for_display(ctx.config)));
            return;
        }
        if (ctx.config.remote_control.token.empty()) {
            ctx.config.remote_control.token = rc::generate_remote_control_token();
            save_config(ctx.config);
        }
        rc::RemoteControlOptions opts;
        opts.port = ctx.config.remote_control.port;
        opts.token = ctx.config.remote_control.token;
        opts.outbound_url = ctx.config.remote_control.outbound_url;
        opts.session_id = current_session_id(ctx);
        std::string error;
        if (!service.start(opts, &error)) {
            emit(ctx, "Failed to start remote control: " + error);
            return;
        }
        set_forward_cursor_to_current_conversation(ctx, service);
        emit(ctx, "Remote control started.\n" +
                  format_remote_control_display(snapshot_for_display(ctx.config)));
        return;
    }
    if (sub == "off") {
        if (!service.running()) {
            clear_active_channel();
            emit(ctx, "Remote control is not running.");
            return;
        }
        std::string warning;
        deactivate_active_channel_best_effort(&warning);
        service.stop();
        std::string message = "Remote control stopped.";
        if (!warning.empty()) message += "\nChannel deactivate warning: " + warning;
        emit(ctx, message);
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
        service.set_outbound_url(rest);
        emit(ctx, "Outbound webhook set to " + rest + "\n" +
                  format_remote_control_display(snapshot_for_display(ctx.config)));
        return;
    }
    if (sub != "show") {
        emit(ctx, "Unknown subcommand. Usage: /remote-control [on|off|show|url <webhook-url>]");
        return;
    }

    emit(ctx, format_remote_control_display(snapshot_for_display(ctx.config)));
}

} // anonymous namespace

std::string format_remote_control_display(const RemoteControlDisplaySnapshot& snap) {
    std::ostringstream oss;
    oss << "Remote control : " << (snap.running ? "ON" : "OFF");
    if (!snap.default_channel.empty()) {
        oss << "\nDefault channel: " << snap.default_channel;
    }
    if (!snap.active_channel.empty()) {
        oss << "\nActive channel : " << snap.active_channel;
    }
    if (snap.running) {
        oss << "\nInbound        : POST http://127.0.0.1:" << snap.port << "/rc/send"
            << "\nToken header   : X-ACECode-RC-Token: " << snap.token
            << "\nBody           : {\"text\": \"<message>\"}"
            << "\nOutbound       : "
            << (snap.outbound_url.empty()
                    ? "(not configured - /remote-control url <webhook-url>)"
                    : snap.outbound_url)
            << "\nStats          : in " << snap.inbound_accepted << " ok / "
            << snap.inbound_rejected << " rejected | out " << snap.outbound_sent
            << " sent / " << snap.outbound_failed << " failed / "
            << snap.outbound_dropped << " dropped";
    } else {
        oss << "\nPort (config)  : " << snap.port
            << "\nOutbound       : "
            << (snap.outbound_url.empty() ? "(not configured)" : snap.outbound_url);
        if (snap.default_channel.empty()) {
            oss << "\nConfigure remote_control.default_channel and remote_control.channels,"
                << " or run /remote-control on for manual webhook pairing.";
        } else {
            oss << "\nRun /rc to activate the default channel."
                << "\nRun /remote-control on for manual webhook pairing.";
        }
    }
    return oss.str();
}

void register_remote_control_command(CommandRegistry& registry) {
    SlashCommand cmd{
        "remote-control",
        "Activate a configured channel plugin or manage manual remote-control webhooks",
        cmd_remote_control,
    };
    registry.register_command(cmd);
    cmd.name = "rc";
    cmd.description = "Alias for /remote-control";
    registry.register_command(cmd);
}

} // namespace acecode
