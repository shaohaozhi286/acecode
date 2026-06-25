#pragma once

// /remote-control(别名 /rc)命令:把当前 TUI 会话托管给外部 channel
// (openspec add-remote-control)。运行时操作委托给 rc::remote_control_service();
// 文案构造抽到 format_remote_control_display() 以便单测覆盖。

#include "commands/command_registry.hpp"

#include <cstdint>
#include <string>

namespace acecode {

struct RemoteControlDisplaySnapshot {
    bool running = false;
    int port = 0;              // running 时为实际监听端口,否则为配置端口
    std::string token;
    std::string outbound_url;
    std::string default_channel;
    std::string active_channel;
    std::uint64_t inbound_accepted = 0;
    std::uint64_t inbound_rejected = 0;
    std::uint64_t outbound_sent = 0;
    std::uint64_t outbound_failed = 0;
    std::uint64_t outbound_dropped = 0;
};

std::string format_remote_control_display(const RemoteControlDisplaySnapshot& snap);

void register_remote_control_command(CommandRegistry& registry);

} // namespace acecode
