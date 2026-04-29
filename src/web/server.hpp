#pragma once

// WebServer: daemon 的 HTTP/WebSocket server(openspec add-web-daemon
// Section 9 + 10)。基于 Crow,内部维护 crow::SimpleApp + 路由注册 +
// per-WS-connection 状态。
//
// 生命周期:
//   - 构造时不监听端口,仅初始化路由与共享状态
//   - run() 阻塞当前线程跑 Crow event loop;直到 stop() 调用或进程收到信号
//   - stop() 触发 Crow app.stop(),同时关闭所有 WebSocket 连接
//
// 线程模型:
//   - HTTP handler 跑在 Crow worker 线程池
//   - WebSocket onmessage 跑在对应连接的 IO 线程
//   - SessionClient::subscribe 注册的 listener 由 AgentLoop worker 线程触发,
//     通过 conn->send_text 发送(send 内部加锁,Crow 保证安全)

#include "../config/config.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace acecode {
class SessionClient;
class SessionRegistry;
class SkillRegistry;
} // namespace acecode

namespace acecode::web {

struct WebServerDeps {
    const WebConfig*           web_cfg = nullptr;
    const DaemonConfig*        daemon_cfg = nullptr;
    AppConfig*                 app_config = nullptr;   // mutable: /api/mcp PUT 改这个
    // 显式 config 落盘路径。空 = 走 save_config(cfg) 默认 ~/.acecode/config.json。
    // daemon worker 启动时填入实际路径;测试 fixture 必须填入临时文件,
    // 否则 PUT /api/mcp 会污染真实用户配置(历史 bug,见 web_server_smoke_test)。
    std::string                config_path;
    std::string                cwd;
    std::string                token;                  // 启动期生成,空 = 不强制 (loopback only)
    std::string                guid;
    std::int64_t               pid = 0;
    std::int64_t               start_time_unix_ms = 0;
    SessionClient*             session_client = nullptr;
    SessionRegistry*           session_registry = nullptr;
    const SkillRegistry*       skill_registry = nullptr;
    bool                       dangerous = false;
};

class WebServer {
public:
    explicit WebServer(WebServerDeps deps);
    ~WebServer();

    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    // 阻塞跑 event loop。返回 0 = 正常退出,非 0 = 启动 / 运行错误。
    int run();

    // 触发 Crow 优雅停服。可由信号 handler / 其它线程调用。
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace acecode::web
