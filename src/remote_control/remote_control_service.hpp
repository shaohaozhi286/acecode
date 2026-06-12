#pragma once

// Remote control 的网络面组装层(openspec add-remote-control)。
//
// 持有:
//   - RemoteControlHub(核心状态机,见 remote_control_hub.hpp)
//   - loopback HTTP listener(Crow,pimpl 隐藏):POST /rc/send + GET /rc/health
//   - WebhookSender(OutboundSender 默认实现:HTTP POST outbound_url)
//
// start() 一把梭:预检端口 → 起 listener → hub.enable;任一步失败整体回滚。
// 进程级单例 remote_control_service() 供 slash command 与 main.cpp 共享 ——
// 与 network::proxy_resolver() 同款惯例,避免在 CommandContext 上加字段。

#include "remote_control_hub.hpp"

#include <memory>
#include <string>

namespace acecode::rc {

struct RemoteControlOptions {
    int port = 28190;          // listener 端口,固定默认值便于 IM 桥配对
    std::string token;         // 必填(由命令层生成或读配置)
    std::string outbound_url;  // 空 = 出站未配置,仅入站
    std::string session_id;    // 出站 payload 的 session 标识
};

struct RemoteControlStatusSnapshot {
    bool running = false;
    int port = 0;
    std::string token;
    std::string outbound_url;
    RemoteControlStats stats;
};

// 生成 32 个十六进制字符的随机 token(std::random_device)。
std::string generate_remote_control_token();

class RemoteControlService {
public:
    RemoteControlService();
    ~RemoteControlService();

    RemoteControlService(const RemoteControlService&) = delete;
    RemoteControlService& operator=(const RemoteControlService&) = delete;

    // 启动 listener + hub。失败返回 false 并填 error,状态完全回滚。
    bool start(const RemoteControlOptions& opts, std::string* error);
    void stop();
    bool running() const;

    // 运行期热更新出站 webhook(/remote-control url <u>)。空串 = 移除出站。
    void set_outbound_url(const std::string& url);

    RemoteControlStatusSnapshot status() const;

    RemoteControlHub& hub() { return hub_; }

private:
    struct Impl;
    RemoteControlHub hub_;
    std::unique_ptr<Impl> impl_;
    bool running_ = false;
    int port_ = 0;
    std::string outbound_url_;
    mutable std::mutex state_mu_;
};

RemoteControlService& remote_control_service();

} // namespace acecode::rc
