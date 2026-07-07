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
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace acecode {
class LlmProvider;
class HookManager;
class PtySessionRegistry;
class SessionClient;
class SessionRegistry;
class SkillRegistry;
class ToolExecutor;
} // namespace acecode

namespace acecode::desktop {
class WorkspaceRegistry;
} // namespace acecode::desktop

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
    std::string                projects_dir;
    std::string                no_workspace_cache_root;
    std::string                logs_dir;
    std::string                feedback_output_dir;
    std::string                token;                  // 启动期生成,空 = 不强制 (loopback only)
    std::string                guid;
    std::int64_t               pid = 0;
    std::int64_t               start_time_unix_ms = 0;
    SessionClient*             session_client = nullptr;
    SessionRegistry*           session_registry = nullptr;
    HookManager*               hook_manager = nullptr;
    ToolExecutor*              tools = nullptr;
    acecode::desktop::WorkspaceRegistry* workspace_registry = nullptr;
    // 非 const:PUT /api/skills/:name 要写 cfg.skills.disabled 后调 set_disabled + reload。
    SkillRegistry*             skill_registry = nullptr;
    // Daemon-global provider handle retained for routes/fixtures that inspect
    // process-level provider state. Current web session model switching is
    // session-scoped through SessionRegistry.
    std::shared_ptr<LlmProvider>* provider = nullptr;
    std::mutex*                    provider_mu = nullptr;
    bool                       native_folder_picker_enabled = false;
    std::function<std::optional<std::string>()> native_folder_picker;
    // POST /api/open-in-explorer 的执行回调:输入 UTF-8 绝对路径,成功返回
    // std::nullopt,失败返回错误信息。null = 端点 501(与 native_folder_picker
    // 同款门控,仅 desktop 壳启动的 daemon 填入;webapp 兼容模式的右键菜单依赖它)。
    std::function<std::optional<std::string>(const std::string&)> open_in_explorer;
    std::function<bool(std::string*)> start_update_command;
    bool                       dangerous = false;
    // Web 控制台 PTY 会话注册表(add-console-dock)。null = 控制台不可用,
    // /api/health 报 console.available=false,PTY 路由 404。
    PtySessionRegistry*        pty_registry = nullptr;
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

    // 登记一个刚 spawn 的子会话:给它挂一个不依赖 WS 客户端订阅的常驻事件
    // 监听器,使其 busy/idle 转换能广播 session_status —— 否则未被任何 WS 连接
    // 订阅的子会话永不广播状态,父会话前端(useSubagentTasks)在 spawn_subagent
    // wait=true 阻塞期间发现不了它(既无 tool_end 也无 status),子代理的权限请求
    // 永远冒泡不到主会话 UI。daemon 的 SubagentToolDeps.on_spawn 在子会话创建后、
    // 首条输入入队前调用本方法。线程安全(可由 agent worker 线程调用)。
    void track_subagent(const std::string& child_session_id);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace acecode::web
